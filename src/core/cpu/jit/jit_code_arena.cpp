// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Process-wide executable storage and epoch-based reclamation. This owner
// depends on fault-range registration, never on compiler or emitter state.

#include "jit_code_arena.hpp"
#include "linear_fault.hpp"
#include "stencils.hpp"
#include "core/memory/jit_region_registry.hpp"
#include "core/memory/host_memory_floor.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <sys/mman.h>
#include <pthread.h>
#include <libkern/OSCacheControl.h>

namespace vitreo::cpu::jit {

// Static member definitions for the shared arena.
JitCodeArena::RetirementObserver JitCodeArena::s_retirement_observer = nullptr;
std::once_flag    JitCodeArena::s_arena_init_flag;
void*             JitCodeArena::s_arena_reservation = nullptr;
usize             JitCodeArena::s_arena_total = 0;
void*             JitCodeArena::s_arena_stub_page = nullptr;
void*             JitCodeArena::s_arena_region = nullptr;
usize             JitCodeArena::s_arena_budget = 0;
usize             JitCodeArena::s_arena_chunk_size = 0;
usize             JitCodeArena::s_arena_island_size = 0;
u32               JitCodeArena::s_arena_chunk_shift = 0;
usize             JitCodeArena::s_arena_chunk_count = 0;
usize             JitCodeArena::s_arena_soft_budget = 0;
std::unique_ptr<JitCodeArena::ArenaChunk[]> JitCodeArena::s_arena_chunks;
std::mutex        JitCodeArena::s_arena_chunk_mutex;
std::atomic<usize> JitCodeArena::s_arena_available{0};
std::atomic<usize> JitCodeArena::s_arena_pending_count{0};
std::atomic<usize> JitCodeArena::s_arena_used_total{0};
std::atomic<u64>   JitCodeArena::s_arena_epoch{1};
std::atomic<u64>   JitCodeArena::s_arena_retire_seq{1};
std::atomic<u64>   JitCodeArena::s_arena_chunk_frees{0};
std::atomic<u64>   JitCodeArena::s_arena_retire_generation{0};
std::atomic<std::atomic<u32>*> JitCodeArena::s_retired_marks{nullptr};
std::atomic<u64>   JitCodeArena::s_retranslated_bytes{0};
std::atomic<u64>   JitCodeArena::s_retranslated_total{0};
u64                JitCodeArena::s_round_retired_bytes = 0;
u64                JitCodeArena::s_thrash_evaluated_seq = 0;
u32                JitCodeArena::s_thrash_streak = 0;
std::atomic<usize> JitCodeArena::s_soft_learned{0};
std::atomic<u32>   JitCodeArena::s_soft_raises{0};
std::mutex         JitCodeArena::s_arena_reclaim_mutex;
std::atomic<u64>   JitCodeArena::s_arena_generation{0};
std::atomic<u64>   JitCodeArena::s_arena_min_generation{1};
std::atomic<u64>   JitCodeArena::s_arena_reclaims{0};
std::atomic<bool>  JitCodeArena::s_arena_reclaim_requested{false};
std::atomic<bool>  JitCodeArena::s_arena_growth_failed{false};
std::atomic<u64>   JitCodeArena::s_arena_gate_closed_ns{0};
std::atomic<u64>   JitCodeArena::s_arena_gate_refusals{0};
std::atomic<u64>   JitCodeArena::s_arena_reclaim_retry_ns{0};
std::atomic<u32>   JitCodeArena::s_arena_drain_failures{0};
std::atomic<u64>   JitCodeArena::s_arena_gate_abandons{0};

namespace {
// Fixed at image startup so an A/B runs both retirement policies from one
// binary: set, a soft-budget round retires only enough for the next chunk.
const bool g_env_arena_retire_one =
    std::getenv("VITREO_JIT_ARENA_RETIRE_ONE") != nullptr;

// Same initial-exec model as GuestMemory's TLB: default TLS was
// _tlv_get_addr on every execute() / thread_holds() during a long walk.
//
// All per-thread fields live in ONE thread_local object on purpose.
// Darwin resolves each distinct thread_local through its own TLV descriptor
// call, so separate variables cost one indirect call each on a path that runs
// per dispatched block; one trivially-destructible, constant-initialised
// struct costs a single lookup and then plain offsets.
struct ArenaLeaseTls {
    u32 lease_depth;      // Nested lease count (thread_holds()).
    u32 allocator_depth;  // Nested arena-allocator count.
    i32 slot;             // Owned occupancy shard, or a kArenaSlot* sentinel.
    u64 epoch;            // Epoch currently published in the shard.
};
constexpr i32 kArenaSlotUnassigned = -1;
constexpr i32 kArenaSlotOverflow   = -2;
thread_local __attribute__((tls_model("initial-exec")))
    ArenaLeaseTls t_arena_lease{0, 0, kArenaSlotUnassigned, 0};

// ---------------------------------------------------------------------------
// Sharded arena occupancy, published as an epoch
// ---------------------------------------------------------------------------
// The lease is taken and dropped around every dispatch chain and every thunk
// call. A single shared counter turns that into an unconditional cross-core
// RMW on one cacheline: with 32 guest threads it was measured at 14.7% of a
// live browser renderer's non-idle CPU, with release() the #2 symbol in the whole
// process, ahead of execute() itself. The cost was pure coherence traffic, not
// blocking.
//
// Each thread instead owns an exclusive, cacheline-isolated occupancy slot.
// Because the nested-lease depth already collapses nested leases, a slot only
// ever moves 0 -> e -> 0 under its owning thread, so a plain store replaces
// the RMW and no line is ever shared.
//
// The value stored is not a flag but the reclamation EPOCH (JitCodeArena::s_arena_epoch)
// the thread read on entry. A retirement bumps the epoch to E; a retired
// chunk's bytes may be recycled once no slot holds a value in (0, E): every
// thread that could have been executing the chunk's code has either left
// native execution (slot 0) or entered again after the retirement (slot >= E)
// and re-validated everything it holds. The reclaimer scans the whole array
// and never waits; a slot that reads 0 < v < E pins the pending chunk exactly
// as a non-zero flag pinned the whole arena before.
//
// Correctness against both the drain gate and a retirement is the same
// Dekker handshake, expressed with an explicit seq_cst fence on each side:
//   entering thread   store(slot, epoch); fence; load(flag / chunk gen / seq)
//   reclaiming thread store(chunk gen 0, seq, epoch); fence; scan every slot
// One of the two must observe the other, so a thread can never enter retired
// code unseen by the retirement that is about to recycle its pages.
constexpr usize kArenaLeaseSlots = 512;
constexpr usize kArenaLeaseLine  = 128;  // Apple silicon cacheline

struct alignas(kArenaLeaseLine) ArenaLeaseSlot {
    std::atomic<u64> epoch{0};
    char pad[kArenaLeaseLine - sizeof(std::atomic<u64>)]{};
};
static_assert(sizeof(ArenaLeaseSlot) == kArenaLeaseLine,
              "one lease slot per cacheline");

ArenaLeaseSlot g_arena_lease_slots[kArenaLeaseSlots];

// Claim bitmap for exclusive slot ownership. Touched once per thread.
constexpr usize kArenaLeaseWords = kArenaLeaseSlots / 64;
std::atomic<u64> g_arena_lease_claimed[kArenaLeaseWords];

// Threads that could not claim a slot (more than kArenaLeaseSlots live JIT
// threads at once) fall back to a shared counter. Correct, just contended —
// and it keeps the drain exact rather than approximate. An overflow holder
// publishes no epoch, so while it is non-zero no pending chunk is freed by
// the epoch rule (the gate fallback still frees once it drains).
std::atomic<u32> g_arena_lease_overflow{0};

/// Return the claimed slot back to the pool when the owning thread exits.
struct ArenaLeaseSlotOwner {
    ~ArenaLeaseSlotOwner() {
        const i32 slot = t_arena_lease.slot;
        t_arena_lease.slot = kArenaSlotUnassigned;
        if (slot < 0) {
            return;
        }
        // A thread cannot be executing arena code while it is being torn
        // down; clear the slot so a stale epoch can never pin a chunk
        // forever, then release the claim for reuse.
        g_arena_lease_slots[static_cast<usize>(slot)].epoch.store(
            0, std::memory_order_release);
        const usize word = static_cast<usize>(slot) / 64;
        const u64 bit = 1ull << (static_cast<usize>(slot) % 64);
        g_arena_lease_claimed[word].fetch_and(~bit, std::memory_order_release);
    }
};
thread_local ArenaLeaseSlotOwner t_arena_lease_slot_owner;

/// Claim this thread's exclusive occupancy slot. Called once per thread.
[[gnu::noinline]] i32 claim_arena_lease_slot() noexcept {
    for (usize word = 0; word < kArenaLeaseWords; ++word) {
        u64 claimed = g_arena_lease_claimed[word].load(
            std::memory_order_relaxed);
        while (claimed != ~0ull) {
            const unsigned free_bit =
                static_cast<unsigned>(__builtin_ctzll(~claimed));
            const u64 bit = 1ull << free_bit;
            if (g_arena_lease_claimed[word].compare_exchange_weak(
                    claimed, claimed | bit, std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                (void)&t_arena_lease_slot_owner;  // force TLS dtor registration
                return static_cast<i32>(word * 64 + free_bit);
            }
        }
    }
    return kArenaSlotOverflow;
}

inline i32 arena_lease_slot(ArenaLeaseTls& tls) noexcept {
    if (tls.slot == kArenaSlotUnassigned) [[unlikely]] {
        tls.slot = claim_arena_lease_slot();
    }
    return tls.slot;
}

/// Publish this thread as an arena user at the current epoch. Hot path: one
/// relaxed load of the epoch and one relaxed store to a privately owned
/// cacheline. The caller performs the seq_cst fence and the rechecks.
inline void publish_arena_occupancy(ArenaLeaseTls& tls) noexcept {
    const i32 slot = arena_lease_slot(tls);
    const u64 epoch =
        JitCodeArena::arena_epoch_relaxed();
    tls.epoch = epoch;
    if (slot >= 0) [[likely]] {
        g_arena_lease_slots[static_cast<usize>(slot)].epoch.store(
            epoch, std::memory_order_relaxed);
    } else {
        g_arena_lease_overflow.fetch_add(1, std::memory_order_acq_rel);
    }
}

/// Drop this thread's arena occupancy. Release ordering keeps every arena
/// read/execute before the slot is observed free.
inline void retract_arena_occupancy(ArenaLeaseTls& tls) noexcept {
    const i32 slot = tls.slot;
    tls.epoch = 0;
    if (slot >= 0) [[likely]] {
        g_arena_lease_slots[static_cast<usize>(slot)].epoch.store(
            0, std::memory_order_release);
    } else {
        g_arena_lease_overflow.fetch_sub(1, std::memory_order_acq_rel);
    }
}

/// True when no thread holds an arena lease. Only the gate reclaimer calls
/// this.
bool arena_leases_drained() noexcept {
    if (g_arena_lease_overflow.load(std::memory_order_acquire) != 0) {
        return false;
    }
    for (const ArenaLeaseSlot& slot : g_arena_lease_slots) {
        if (slot.epoch.load(std::memory_order_acquire) != 0) {
            return false;
        }
    }
    return true;
}

/// Oldest epoch any thread currently inside the arena entered at; 0 when an
/// overflow holder (which publishes no epoch) is inside; ~0 when nobody is.
u64 arena_oldest_inside_epoch() noexcept {
    if (g_arena_lease_overflow.load(std::memory_order_acquire) != 0) {
        return 0;
    }
    u64 oldest = ~u64{0};
    for (const ArenaLeaseSlot& slot : g_arena_lease_slots) {
        const u64 epoch = slot.epoch.load(std::memory_order_acquire);
        if (epoch != 0 && epoch < oldest) oldest = epoch;
    }
    return oldest;
}
}  // namespace

JitCodeArena::ArenaReadLease::ArenaReadLease() noexcept {
    (void)acquire();
}

JitCodeArena::ArenaReadLease::ArenaReadLease(StableExecution) noexcept
{ (void)acquire(); }

JitCodeArena::ArenaReadLease::ArenaReadLease(DeferAcquire) noexcept {}

JitCodeArena::ArenaReadLease::~ArenaReadLease() {
    release();
}

bool JitCodeArena::ArenaReadLease::thread_holds() noexcept {
    return t_arena_lease.lease_depth != 0;
}

void* JitCodeArena::ArenaReadLease::thread_state() noexcept {
    return &t_arena_lease;
}

bool JitCodeArena::ArenaReadLease::thread_holds_at(const void* state) noexcept {
    return static_cast<const ArenaLeaseTls*>(state)->lease_depth != 0;
}

void JitCodeArena::ArenaReadLease::refresh_epoch() noexcept {
    refresh_epoch_at(&t_arena_lease);
}

void JitCodeArena::ArenaReadLease::refresh_epoch_at(void* state) noexcept {
    ArenaLeaseTls& tls = *static_cast<ArenaLeaseTls*>(state);
    if (tls.allocator_depth == 0 || tls.slot < 0) {
        return;  // Not inside, or an overflow holder (no epoch to move).
    }
    const u64 epoch = s_arena_epoch.load(std::memory_order_relaxed);
    if (epoch == tls.epoch) {
        return;
    }
    tls.epoch = epoch;
    g_arena_lease_slots[static_cast<usize>(tls.slot)].epoch.store(
        epoch, std::memory_order_relaxed);
    // Same handshake as acquire(): the caller's re-validation loads (retire
    // sequence, chunk generation) must follow the publication.
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

bool JitCodeArena::ArenaReadLease::acquire() noexcept {
    if (m_acquired) {
        return true;
    }
    ArenaLeaseTls& tls = t_arena_lease;
    if (tls.allocator_depth != 0) {
        ++tls.allocator_depth;
        ++tls.lease_depth;
        m_acquired = true;
        return true;
    }
    // Closing the gate prevents new native execution while the reclaimer
    // waits. The second check closes the race where reclamation was requested
    // after our first check but before we published ourselves as active.
    if (s_arena_reclaim_requested.load(std::memory_order_acquire)) {
        s_arena_gate_refusals.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    publish_arena_occupancy(tls);
    // Dekker: the reclaimer sets the flag (or retires a chunk), fences, then
    // scans the slots. The fence here orders our publication ahead of the
    // recheck, so a reclaim that misses our slot is one whose flag — and
    // whose retirement — we are guaranteed to see.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (s_arena_reclaim_requested.load(std::memory_order_relaxed)) {
        retract_arena_occupancy(tls);
        s_arena_gate_refusals.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    tls.allocator_depth = 1;
    tls.lease_depth = 1;
    m_acquired = true;
    return true;
}

void JitCodeArena::ArenaReadLease::release() noexcept {
    if (!m_acquired) {
        return;
    }
    m_acquired = false;
    ArenaLeaseTls& tls = t_arena_lease;
    if (tls.lease_depth != 0) {
        --tls.lease_depth;
    }
    if (tls.allocator_depth > 1) {
        --tls.allocator_depth;
        return;
    }
    tls.allocator_depth = 0;
    // No flag check and no notify: reclamation attempts poll the slots at
    // dispatcher safe points. The hot exit remains a single release store
    // to a private line, observed by a subsequent attempt.
    retract_arena_occupancy(tls);
}

bool JitCodeArena::map_arena_reservation(usize budget) {
    constexpr usize kGuardSize = 16384;
    if (budget == 0) {
        return false;
    }

    // ONE reservation for the whole budget, mapped once and never remapped as
    // a whole. Chunks are recycled inside it.
    //
    // Individual chunks must NOT be mapped and unmapped per retirement: the
    // signal handler's JIT code-range table publishes an immutable snapshot per
    // change and never reuses a snapshot slot (a handler may still hold an old
    // pointer), so its pool is a hard, monotonic per-process budget. Churning
    // ranges exhausts it, after which registration fails, native compilation is
    // disabled process-wide, and the guest silently drops to the interpreter
    // forever. Registering the reservation exactly once removes that ceiling.
    void* reservation = MAP_FAILED;
    usize arena_size = budget;
    usize total_size = 0;
    for (usize request = budget; request >= 16ull * 1024 * 1024;
         request /= 2) {
        total_size = kGuardSize + kSharedStubPageSize + request + kGuardSize;
        reservation = mmap(nullptr, total_size,
                           PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT,
                           -1, 0);
        if (reservation != MAP_FAILED) {
            arena_size = request;
            break;
        }
        fprintf(stderr, "[jit] WARNING: MAP_JIT reservation failed "
                        "(errno=%d, size=%zuMB)\n",
                errno, request / (1024 * 1024));
    }
    if (reservation == MAP_FAILED) {
        return false;
    }

    mprotect(reservation, kGuardSize, PROT_NONE);
    mprotect(static_cast<u8*>(reservation) + kGuardSize + kSharedStubPageSize +
                 arena_size,
             kGuardSize, PROT_NONE);

    void* jit_region =
        static_cast<u8*>(reservation) + kGuardSize + kSharedStubPageSize;
    const uintptr_t reg_start = reinterpret_cast<uintptr_t>(jit_region);
    const uintptr_t reg_end = reg_start + arena_size;
    if (!note_jit_code_range(reg_start, reg_end)) {
        munmap(reservation, total_size);
        return false;
    }

    s_arena_reservation = reservation;
    s_arena_total = total_size;
    s_arena_stub_page = static_cast<u8*>(reservation) + kGuardSize;
    s_arena_region = jit_region;
    s_arena_budget = arena_size;
    JitRegionRegistry::instance().add(reg_start, reg_end);
    return true;
}

usize JitCodeArena::arena_chunk_size_config() noexcept {
    usize size = kArenaChunkSize;
    if (const char* env = std::getenv("VITREO_JIT_ARENA_CHUNK_MB")) {
        char* end = nullptr;
        const unsigned long long mb = std::strtoull(env, &end, 10);
        if (end != env && mb >= 1ull && mb <= 512ull) {
            size = static_cast<usize>(mb * 1024ull * 1024ull);
        }
    }
    // Power of two: chunk lookup on the dispatch path is a shift.
    usize pow2 = 1;
    while (pow2 * 2 <= size) pow2 *= 2;
    return pow2;
}

void JitCodeArena::ensure_initialized() {
    std::call_once(s_arena_init_flag, init_shared_arena);
}

void JitCodeArena::init_shared_arena() {
    usize budget = kArenaSize;
    if (const char* env = std::getenv("VITREO_JIT_ARENA_MB")) {
        char* end = nullptr;
        const unsigned long long mb = std::strtoull(env, &end, 10);
        if (end != env && mb >= 8ull && mb <= 8192ull) {
            budget = static_cast<usize>(mb * 1024ull * 1024ull);
        }
    }

    if (!map_arena_reservation(budget)) {
        return;
    }
    usize chunk_size = arena_chunk_size_config();
    while (chunk_size > s_arena_budget) chunk_size /= 2;
    u32 shift = 0;
    while ((usize{1} << shift) < chunk_size) ++shift;
    s_arena_chunk_size = chunk_size;
    s_arena_chunk_shift = shift;
    // 1/8 of a chunk (1 MiB of 8 MiB): far-branch veneers and materialized
    // lazy slow paths. Measured on Steam, ~12% of x64 memory sites
    // materialize (~85 B each); 1/64 filled within seconds and sent ~320k
    // blocks to an eager recompile, 1/8 left ~1.8k. Island pages nothing
    // writes are never resident. VITREO_JIT_ARENA_ISLAND_DIV overrides.
    usize island_div = 8;
    if (const char* env = std::getenv("VITREO_JIT_ARENA_ISLAND_DIV")) {
        const unsigned long long d = std::strtoull(env, nullptr, 10);
        if (d >= 4 && d <= 1024) island_div = static_cast<usize>(d);
    }
    s_arena_island_size = chunk_size / island_div;
    s_arena_chunk_count = s_arena_budget / chunk_size;
    s_arena_chunks = std::make_unique<ArenaChunk[]>(s_arena_chunk_count);
    s_arena_available.store(s_arena_chunk_count, std::memory_order_release);
    s_arena_min_generation.store(
        s_arena_generation.load(std::memory_order_relaxed) + 1,
        std::memory_order_release);
    usize soft = kArenaSoftBudget;
    if (const char* env = std::getenv("VITREO_JIT_ARENA_SOFT_MB")) {
        char* end = nullptr;
        const unsigned long long mb = std::strtoull(env, &end, 10);
        if (end != env && mb <= 8192ull) {
            soft = mb == 0 ? s_arena_budget
                           : static_cast<usize>(mb * 1024ull * 1024ull);
        }
    }
    s_arena_soft_budget = std::clamp(soft, chunk_size, s_arena_budget);
    fprintf(stderr,
            "[jit] shared arena: %zuMB MAP_JIT at %p, %zu x %zuMB chunks, "
            "soft budget %zuMB\n",
            s_arena_budget / (1024 * 1024), s_arena_region,
            s_arena_chunk_count, s_arena_chunk_size / (1024 * 1024),
            s_arena_soft_budget / (1024 * 1024));
}

// ---------------------------------------------------------------------------
// Chunk bookkeeping (all under s_arena_chunk_mutex)
// ---------------------------------------------------------------------------

void JitCodeArena::note_chunk_hit(const void* code, u32 n) noexcept {
    if (s_arena_region == nullptr || code == nullptr) return;
    const usize index = arena_chunk_index(code);
    if (index >= s_arena_chunk_count) return;
    s_arena_chunks[index].hits.fetch_add(n, std::memory_order_relaxed);
}

void JitCodeArena::arena_recount_locked() noexcept {
    usize available = 0;
    for (usize i = 0; i < s_arena_chunk_count; ++i) {
        const ArenaChunk& chunk = s_arena_chunks[i];
        if (chunk.pending_free || chunk.owned) continue;
        if (chunk.generation.load(std::memory_order_relaxed) == 0) {
            ++available;
        } else if (chunk_code_capacity() -
                       std::min(chunk.used.load(std::memory_order_relaxed),
                                chunk_code_capacity()) >=
                   kArenaMinUsefulBytes) {
            ++available;
        }
    }
    s_arena_available.store(available, std::memory_order_release);
}

void JitCodeArena::arena_refresh_min_generation_locked() noexcept {
    u64 oldest = ~u64{0};
    for (usize i = 0; i < s_arena_chunk_count; ++i) {
        const u64 generation =
            s_arena_chunks[i].generation.load(std::memory_order_relaxed);
        if (generation != 0 && generation < oldest) oldest = generation;
    }
    if (oldest == ~u64{0}) {
        oldest = s_arena_generation.load(std::memory_order_relaxed) + 1;
    }
    s_arena_min_generation.store(oldest, std::memory_order_release);
}

usize JitCodeArena::arena_live_bytes_locked() noexcept {
    // Whole chunks, not bytes used: MADV_FREE_REUSE charges a recycled
    // chunk's surviving pages back at once, so a live chunk may cost all of
    // its pages however little of it the current generation has written.
    usize live = 0;
    for (usize i = 0; i < s_arena_chunk_count; ++i) {
        const ArenaChunk& chunk = s_arena_chunks[i];
        if (chunk.pending_free ||
            chunk.generation.load(std::memory_order_relaxed) == 0) {
            continue;
        }
        live += s_arena_chunk_size;
    }
    return live;
}

usize JitCodeArena::arena_effective_soft_budget() noexcept {
    usize soft = s_arena_soft_budget;
    if (soft >= s_arena_budget) return soft;  // Disabled.
    switch (memory::host_memory_readings().pressure) {
        case memory::HostMemoryPressure::warn:     soft /= 2; break;
        case memory::HostMemoryPressure::critical: soft /= 4; break;
        case memory::HostMemoryPressure::normal:   break;
    }
    soft = std::max(soft, std::min(s_arena_soft_budget, kArenaSoftBudgetFloor));
    // Pressure never takes a process below the budget its own working set
    // was measured to need: below it a round retires live code, which is
    // translated again into a fresh chunk at once, so the footprint does not
    // fall -- only CPU is spent. The trade-off is explicit: under critical
    // pressure such a process keeps its working set resident (or swapped)
    // rather than recompiling it without end.
    return std::max(soft, s_soft_learned.load(std::memory_order_relaxed));
}

namespace {
// One 64-bit key per (guest address, guest code bytes): the slot comes from
// its top kRetiredMarkBits bits, the 24-bit fingerprint from bits 8..31.
inline u64 retired_mark_key(u64 addr, u64 code_hash) noexcept {
    u64 h = addr * 0x9E3779B97F4A7C15ull ^ code_hash;
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
    h *= 0xC4CEB9FE1A85EC53ull;
    h ^= h >> 33;
    return h;
}
inline u32 retired_mark_tag(u64 retire_seq) noexcept {
    return static_cast<u32>(retire_seq % 255u) + 1u;  // 1..255; 0 = empty
}
}  // namespace

void JitCodeArena::note_retired_translation(u64 addr, u64 code_hash) noexcept {
    std::atomic<u32>* const marks =
        s_retired_marks.load(std::memory_order_acquire);
    if (marks == nullptr) return;
    const u64 key = retired_mark_key(addr, code_hash);
    const u32 tag = retired_mark_tag(
        s_arena_retire_seq.load(std::memory_order_relaxed));
    const u32 entry = (static_cast<u32>(key) & ~u32{0xFF}) | tag;
    marks[key >> (64 - kRetiredMarkBits)].store(entry,
                                                std::memory_order_relaxed);
}

void JitCodeArena::note_fresh_translation(u64 addr, u64 code_hash,
                                          usize bytes) noexcept {
    std::atomic<u32>* const marks =
        s_retired_marks.load(std::memory_order_acquire);
    if (marks == nullptr) return;
    const u64 key = retired_mark_key(addr, code_hash);
    std::atomic<u32>& slot = marks[key >> (64 - kRetiredMarkBits)];
    u32 entry = slot.load(std::memory_order_relaxed);
    if (entry == 0 ||
        (entry & ~u32{0xFF}) != (static_cast<u32>(key) & ~u32{0xFF})) {
        return;
    }
    const u32 now = retired_mark_tag(
        s_arena_retire_seq.load(std::memory_order_relaxed));
    const u32 age = (now + 255u - (entry & 0xFFu)) % 255u;
    if (age > kThrashWindowRounds) return;
    // Count a retirement once, however many compilers re-emit it.
    if (!slot.compare_exchange_strong(entry, 0, std::memory_order_relaxed)) {
        return;
    }
    s_retranslated_bytes.fetch_add(bytes, std::memory_order_relaxed);
    s_retranslated_total.fetch_add(bytes, std::memory_order_relaxed);
}

void JitCodeArena::reset_soft_budget_learning() noexcept {
    std::lock_guard<std::mutex> lock(s_arena_chunk_mutex);
    s_soft_learned.store(0, std::memory_order_relaxed);
    s_soft_raises.store(0, std::memory_order_relaxed);
    s_retranslated_bytes.store(0, std::memory_order_relaxed);
    s_retranslated_total.store(0, std::memory_order_relaxed);
    s_round_retired_bytes = 0;
    s_thrash_evaluated_seq = s_arena_retire_seq.load(std::memory_order_relaxed);
    s_thrash_streak = 0;
    if (std::atomic<u32>* marks = s_retired_marks.load(std::memory_order_acquire)) {
        for (usize i = 0; i < (usize{1} << kRetiredMarkBits); ++i) {
            marks[i].store(0, std::memory_order_relaxed);
        }
    }
}

bool JitCodeArena::arena_learn_from_thrash_locked() {
    // Judge each round once, on what happened between it and now.
    const u64 seq = s_arena_retire_seq.load(std::memory_order_relaxed);
    if (seq == s_thrash_evaluated_seq || s_round_retired_bytes == 0) {
        return false;
    }
    s_thrash_evaluated_seq = seq;
    const u64 again = s_retranslated_bytes.load(std::memory_order_relaxed);
    const u64 retired = s_round_retired_bytes;
    if (again * 2 < retired) {
        s_thrash_streak = 0;
        return false;
    }
    if (++s_thrash_streak < kThrashRounds) return false;
    s_thrash_streak = 0;
    const usize current = arena_effective_soft_budget();
    if (current >= s_arena_budget) return false;
    const usize raised = std::min(s_arena_budget, current * 2);
    s_soft_learned.store(raised, std::memory_order_relaxed);
    s_soft_raises.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[jit] arena soft budget raised %zuMB -> %zuMB: %lluKB of "
                 "the %lluKB the last round retired was translated again "
                 "(%u rounds in a row; the live working set exceeds the "
                 "budget)\n",
                 current / (1024 * 1024), raised / (1024 * 1024),
                 static_cast<unsigned long long>(again / 1024),
                 static_cast<unsigned long long>(retired / 1024),
                 kThrashRounds);
    return true;
}

std::vector<usize> JitCodeArena::arena_victim_order_locked(bool force) {
    // Candidates: live, not already pending, and — unless forced — not a
    // compiler's current bump chunk. Least recently hit first, oldest
    // generation as the tie-break. With `force` the oldest live chunk goes
    // first regardless (the explicit-request semantics).
    //
    // A chunk armed since the previous retirement round goes last: its hits
    // have had no sampling period yet, while a dead chunk's still decay by
    // halves, so ranking it on hits alone retired the newest code first.
    const u64 young_after =
        s_arena_retire_generation.load(std::memory_order_relaxed);
    struct Candidate { usize index; u64 hits; u64 generation; };
    std::vector<Candidate> candidates;
    for (usize i = 0; i < s_arena_chunk_count; ++i) {
        const ArenaChunk& chunk = s_arena_chunks[i];
        const u64 generation = chunk.generation.load(std::memory_order_relaxed);
        if (generation == 0 || chunk.pending_free) continue;
        if (chunk.owned && !force) continue;
        candidates.push_back(Candidate{
            i, chunk.hits.load(std::memory_order_relaxed), generation});
    }
    std::sort(candidates.begin(), candidates.end(),
              [force, young_after](const Candidate& a, const Candidate& b) {
                  if (force) return a.generation < b.generation;
                  const bool a_young = a.generation > young_after;
                  const bool b_young = b.generation > young_after;
                  if (a_young != b_young) return b_young;
                  if (a.hits != b.hits) return a.hits < b.hits;
                  return a.generation < b.generation;
              });
    std::vector<usize> order;
    order.reserve(candidates.size());
    for (const Candidate& c : candidates) order.push_back(c.index);
    return order;
}

void JitCodeArena::arena_free_chunk_locked(usize index) noexcept {
    ArenaChunk& chunk = s_arena_chunks[index];
    // Recycling a chunk drops the physical pages its retired code occupied.
    // A fresh MAP_FIXED|MAP_JIT mapping cannot do it: Darwin allows one
    // MAP_JIT entry per process and refuses the second with EINVAL (measured
    // 2026-10-18), and plain MADV_FREE leaves the pages charged to
    // phys_footprint until the kernel happens to reclaim them.
    // MADV_FREE_REUSABLE removes them from the footprint at once;
    // MADV_FREE_REUSE when the chunk is re-armed restores
    // the accounting and stops the kernel discarding what is written next.
    // The virtual range and its signal-handler registration are untouched.
    //
    // Reusing an address is only safe because retirement already stored 0
    // to the chunk generation (so no published pointer passes
    // arena_code_live()), and because the caller has established that no
    // thread can still be executing inside it — either the epoch rule or a
    // full drain. Without both, this would silently execute the wrong code
    // rather than fault.
    u8* const base =
        static_cast<u8*>(s_arena_region) + index * s_arena_chunk_size;
    retire_jit_rip_range(reinterpret_cast<uintptr_t>(base),
                         reinterpret_cast<uintptr_t>(base) +
                             s_arena_chunk_size);
    if (madvise(base, s_arena_chunk_size, MADV_FREE_REUSABLE) == 0) {
        chunk.reusable = true;
    } else {
        static std::atomic<bool> s_logged{false};
        if (!s_logged.exchange(true, std::memory_order_relaxed)) {
            std::fprintf(stderr,
                         "[jit] WARNING: MADV_FREE_REUSABLE on a retired "
                         "arena chunk failed (errno=%d); its pages stay "
                         "resident until reused\n",
                         errno);
        }
    }
    chunk.pending_free = false;
    chunk.retired_epoch = 0;
    chunk.retired_ns = 0;
    chunk.used.store(0, std::memory_order_relaxed);
    chunk.hits.store(0, std::memory_order_relaxed);
    chunk.island_used = 0;
    chunk.incoming.clear();
    chunk.incoming.shrink_to_fit();
    s_arena_pending_count.fetch_sub(1, std::memory_order_relaxed);
    s_arena_chunk_frees.fetch_add(1, std::memory_order_relaxed);
}

void JitCodeArena::arena_note_incoming_link(const void* target_code,
                                           const void* source_code,
                                           u64 source_generation,
                                           u32* gen_site,
                                           u32 restore_word) {
    if (s_arena_region == nullptr) return;
    const usize target = arena_chunk_index(target_code);
    const usize source = arena_chunk_index(source_code);
    if (target >= s_arena_chunk_count || source >= s_arena_chunk_count ||
        target == source) {
        // Same chunk: the site dies with its target; nothing to unpatch.
        return;
    }
    std::lock_guard<std::mutex> lock(s_arena_chunk_mutex);
    s_arena_chunks[target].incoming.push_back(
        ArenaIncomingLink{gen_site, source_generation, restore_word});
}

extern constinit thread_local __attribute__((tls_model("initial-exec")))
    bool t_jit_writable;

bool JitCodeArena::arena_write_word_if_live(u32* site, u64 source_generation,
                                            u32 word) {
    if (s_arena_region == nullptr || site == nullptr) return false;
    std::lock_guard<std::mutex> lock(s_arena_chunk_mutex);
    if (!arena_code_live(site, source_generation)) return false;
    if (site[0] == word) return true;
    // Leave the thread's W^X state as the caller had it (jit_execute.cpp).
    if (!t_jit_writable) pthread_jit_write_protect_np(0);
    __atomic_store_n(site, word, __ATOMIC_RELEASE);
    if (!t_jit_writable) pthread_jit_write_protect_np(1);
    sys_icache_invalidate(site, sizeof(u32));
    return true;
}

void* JitCodeArena::alloc_far_branch(const void* source_code,
                                     u64 source_generation, usize bytes) {
    if (s_arena_region == nullptr || source_code == nullptr ||
        s_arena_island_size == 0) {
        return nullptr;
    }
    const usize index = arena_chunk_index(source_code);
    if (index >= s_arena_chunk_count) return nullptr;
    bytes = (bytes + 15) & ~usize{15};
    std::lock_guard<std::mutex> lock(s_arena_chunk_mutex);
    ArenaChunk& chunk = s_arena_chunks[index];
    // Checked under the mutex retirement takes, so the island cannot be
    // recycled between this check and the caller's write (the caller also
    // holds an ArenaReadLease, which keeps the bytes from being freed).
    if (chunk.generation.load(std::memory_order_acquire) != source_generation ||
        chunk.island_used + bytes > s_arena_island_size) {
        return nullptr;
    }
    chunk.island_used += bytes;
    return static_cast<u8*>(s_arena_region) + (index + 1) * s_arena_chunk_size -
           chunk.island_used;
}

void JitCodeArena::release_chunk(Cursor& cursor, bool seal) noexcept {
    if (cursor.chunk < 0) return;
    std::lock_guard<std::mutex> lock(s_arena_chunk_mutex);
    ArenaChunk& chunk = s_arena_chunks[static_cast<usize>(cursor.chunk)];
    if (chunk.generation.load(std::memory_order_relaxed) ==
        cursor.generation) {
        chunk.owned = false;
        if (seal) chunk.used.store(s_arena_chunk_size, std::memory_order_relaxed);
    }
    cursor.chunk = -1;
    cursor.generation = 0;
    cursor.used = 0;
    arena_recount_locked();
}

bool JitCodeArena::take_chunk(Cursor& cursor) {
    if (s_arena_region == nullptr ||
        s_arena_growth_failed.load(std::memory_order_acquire)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(s_arena_chunk_mutex);
    if (cursor.chunk >= 0) {
        // Whatever we held is given back (full, or retired under us).
        ArenaChunk& old = s_arena_chunks[static_cast<usize>(cursor.chunk)];
        if (old.generation.load(std::memory_order_relaxed) ==
            cursor.generation) {
            old.owned = false;
        }
        cursor.chunk = -1;
        cursor.generation = 0;
        cursor.used = 0;
        cursor.win_hot = 0;
        cursor.win_cold = 0;
    }

    const auto take = [&cursor](usize index, u64 generation, usize used) {
        ArenaChunk& chunk = s_arena_chunks[index];
        chunk.owned = true;
        cursor.chunk = static_cast<i32>(index);
        cursor.generation = generation;
        cursor.used = used;
        cursor.win_hot = 0;
        cursor.win_cold = 0;
        arena_recount_locked();
        return true;
    };

    // 1. A live chunk another compiler gave back with room left: resume it.
    //    Most room first, so a nearly full leftover is not preferred over a
    //    nearly empty one.
    {
        usize best = s_arena_chunk_count;
        usize best_room = 0;
        for (usize i = 0; i < s_arena_chunk_count; ++i) {
            const ArenaChunk& chunk = s_arena_chunks[i];
            if (chunk.owned || chunk.pending_free) continue;
            if (chunk.generation.load(std::memory_order_relaxed) == 0) continue;
            const usize used = std::min(chunk.used.load(std::memory_order_relaxed),
                                        chunk_code_capacity());
            const usize room = chunk_code_capacity() - used;
            if (room >= kArenaMinUsefulBytes && room > best_room) {
                best = i;
                best_room = room;
            }
        }
        if (best != s_arena_chunk_count) {
            return take(best,
                        s_arena_chunks[best].generation.load(
                            std::memory_order_relaxed),
                        s_arena_chunks[best].used.load(
                            std::memory_order_relaxed));
        }
    }

    // 2. A free chunk (never used, or recycled). Arming one grows the live
    //    footprint by up to a chunk, so first retire the coldest live chunks
    //    until it fits the soft budget; the sweep then releases their pages
    //    as soon as the epoch rule allows. Owned chunks are never taken
    //    here, so N compilers can hold the footprint at N chunks.
    //    A round retires down to soft - max(soft/8, one chunk), not just
    //    enough for this chunk: every round makes every thread purge its
    //    caches and walk its blocks (synchronize_arena_generation) and takes
    //    the memo lock exclusively, so one victim per take made that walk
    //    run once per armed chunk at the budget.
    {
        usize soft = arena_effective_soft_budget();
        usize live = arena_live_bytes_locked();
        // Before retiring more: if the previous rounds' victims came straight
        // back, the budget is below the working set and is raised instead.
        if (live + s_arena_chunk_size > soft &&
            arena_learn_from_thrash_locked()) {
            soft = arena_effective_soft_budget();
        }
        if (live + s_arena_chunk_size > soft) {
            const usize margin = std::max(soft / 8, s_arena_chunk_size);
            const usize target = g_env_arena_retire_one
                                     ? soft - std::min(soft, s_arena_chunk_size)
                                     : soft - std::min(soft, margin);
            std::vector<usize> victims;
            for (const usize index : arena_victim_order_locked(false)) {
                if (live <= target) break;
                live -= s_arena_chunk_size;
                victims.push_back(index);
            }
            if (!victims.empty()) {
                s_arena_chunk_mutex.unlock();
                (void)retire_arena_victims(std::move(victims));
                (void)sweep_retired_chunks();
                s_arena_chunk_mutex.lock();
            }
        }
    }
    //    Keep the pool from running dry: when few remain, retire the coldest
    //    live chunks now so their epoch wait overlaps with the chunks still
    //    being consumed.
    const auto count_free = [] {
        usize free = 0;
        for (usize i = 0; i < s_arena_chunk_count; ++i) {
            const ArenaChunk& chunk = s_arena_chunks[i];
            if (!chunk.pending_free &&
                chunk.generation.load(std::memory_order_relaxed) == 0) {
                ++free;
            }
        }
        return free;
    };
    const usize low_water =
        std::max<usize>(1, std::min<usize>(JitCodeArena::s_arena_chunk_count / 16, 32));
    usize free = count_free();
    if (free <= low_water) {
        const usize pending = JitCodeArena::s_arena_pending_count.load(
            std::memory_order_relaxed);
        const usize want = low_water + 1 > free + pending
                               ? low_water + 1 - free - pending
                               : 0;
        if (want != 0) {
            // Retire, then recycle whatever the epoch rule already permits
            // (a chunk retired at an earlier take may be free by now).
            // Both drop and re-take the chunk mutex.
            JitCodeArena::s_arena_chunk_mutex.unlock();
            (void)JitCodeArena::retire_arena_chunks(want, /*force=*/false);
            (void)JitCodeArena::sweep_retired_chunks();
            JitCodeArena::s_arena_chunk_mutex.lock();
        } else if (pending != 0) {
            JitCodeArena::s_arena_chunk_mutex.unlock();
            (void)JitCodeArena::sweep_retired_chunks();
            JitCodeArena::s_arena_chunk_mutex.lock();
        }
        free = count_free();
    }
    if (free != 0) {
        for (usize i = 0; i < JitCodeArena::s_arena_chunk_count; ++i) {
            JitCodeArena::ArenaChunk& chunk = JitCodeArena::s_arena_chunks[i];
            if (chunk.pending_free ||
                chunk.generation.load(std::memory_order_relaxed) != 0) {
                continue;
            }
            if (chunk.reusable) {
                if (madvise(static_cast<u8*>(s_arena_region) +
                                i * s_arena_chunk_size,
                            s_arena_chunk_size, MADV_FREE_REUSE) != 0) {
                    static std::atomic<bool> s_logged{false};
                    if (!s_logged.exchange(true, std::memory_order_relaxed)) {
                        std::fprintf(stderr,
                                     "[jit] WARNING: MADV_FREE_REUSE on arena "
                                     "chunk %zu failed (errno=%d); chunk "
                                     "left unarmed\n",
                                     i, errno);
                    }
                    continue;
                }
                chunk.reusable = false;
            }
            const u64 generation =
                JitCodeArena::s_arena_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
            chunk.used.store(0, std::memory_order_relaxed);
            chunk.hits.store(0, std::memory_order_relaxed);
            chunk.island_used = 0;
            chunk.generation.store(generation, std::memory_order_release);
            arena_refresh_min_generation_locked();
            return take(i, generation, 0);
        }
    }

    // 3. Nothing to take: every chunk is live or still pending its epoch.
    //    The compile is deferred; the next safe-point sweep normally frees
    //    a pending chunk. If pending chunks have been stuck for a while, a
    //    thread is not leaving native code: fall back to the drain gate,
    //    which closes native entry until every lease is released and then
    //    frees everything pending (with its own backoff if that never
    //    happens).
    constexpr u64 kStuckPendingNs = 50ull * 1000ull * 1000ull;  // 50 ms
    const u64 now = steady_now_ns();
    bool stuck = false;
    for (usize i = 0; i < s_arena_chunk_count; ++i) {
        const ArenaChunk& chunk = s_arena_chunks[i];
        if (chunk.pending_free && chunk.retired_ns != 0 &&
            now - chunk.retired_ns > kStuckPendingNs) {
            stuck = true;
            break;
        }
    }
    if (stuck ||
        JitCodeArena::s_arena_pending_count.load(std::memory_order_relaxed) == 0) {
        request_arena_reclaim();
    }
    return false;
}

bool JitCodeArena::has_room(Cursor& cursor) noexcept {
    if (s_arena_region == nullptr) {
        return false;
    }
    if (cursor.chunk >= 0 &&
        s_arena_chunks[static_cast<usize>(cursor.chunk)].generation.load(
            std::memory_order_acquire) == cursor.generation &&
        cursor.used + kArenaMinUsefulBytes <= chunk_code_capacity()) {
        return true;
    }
    if (s_arena_available.load(std::memory_order_acquire) != 0) {
        return true;
    }
    return take_chunk(cursor);
}

usize JitCodeArena::retire_arena_chunks(usize count, bool force) {
    if (s_arena_region == nullptr || count == 0) return 0;
    std::vector<usize> victims;
    {
        std::lock_guard<std::mutex> lock(s_arena_chunk_mutex);
        victims = arena_victim_order_locked(force);
        if (victims.size() > count) victims.resize(count);
    }
    return retire_arena_victims(std::move(victims));
}

bool JitCodeArena::retire_arena_chunk_containing(const void* code) {
    if (s_arena_region == nullptr || code == nullptr) return false;
    const usize index = arena_chunk_index(code);
    if (index >= s_arena_chunk_count) return false;
    {
        std::lock_guard<std::mutex> lock(s_arena_chunk_mutex);
        const ArenaChunk& chunk = s_arena_chunks[index];
        if (chunk.pending_free ||
            chunk.generation.load(std::memory_order_relaxed) == 0) {
            return false;
        }
    }
    return retire_arena_victims(std::vector<usize>{index}) == 1;
}

usize JitCodeArena::retire_arena_victims(std::vector<usize> victims) {
    if (victims.empty()) return 0;
    usize live_after = 0;
    {
        std::lock_guard<std::mutex> lock(s_arena_chunk_mutex);
        // Re-validate under the lock: a victim chosen earlier may have been
        // retired by a racing round.
        std::erase_if(victims, [](usize index) {
            const ArenaChunk& chunk = s_arena_chunks[index];
            return chunk.pending_free ||
                   chunk.generation.load(std::memory_order_relaxed) == 0;
        });
        if (victims.empty()) return 0;
        // Thrash accounting: this round's retired bytes are what the
        // re-translations counted until the next round are compared with.
        // The marks table exists only where a soft budget can retire.
        {
            u64 retired_bytes = 0;
            for (const usize index : victims) {
                retired_bytes += std::min(
                    s_arena_chunks[index].used.load(std::memory_order_relaxed),
                    chunk_code_capacity());
            }
            s_round_retired_bytes = retired_bytes;
            s_retranslated_bytes.store(0, std::memory_order_relaxed);
            if (s_arena_soft_budget < s_arena_budget &&
                s_retired_marks.load(std::memory_order_relaxed) == nullptr) {
                const usize bytes =
                    (usize{1} << kRetiredMarkBits) * sizeof(std::atomic<u32>);
                void* table = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (table != MAP_FAILED) {
                    // Zero-filled pages are empty slots.
                    s_retired_marks.store(
                        static_cast<std::atomic<u32>*>(table),
                        std::memory_order_release);
                }
            }
        }
        // Step 1: no pointer into a victim passes arena_code_live() from
        // here on. Stored before anything else so that a thread which
        // misses this retirement in its slot scan (Dekker) still refuses
        // the code.
        for (const usize index : victims) {
            ArenaChunk& chunk = s_arena_chunks[index];
            chunk.generation.store(0, std::memory_order_release);
            s_arena_used_total.fetch_sub(
                chunk.used.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
        }
        // Step 2: fail every direct link INTO a victim closed again. The
        // MOVZ generation immediate at the site goes back to 0, which the
        // runtime compare (LDR link_generation; EOR; CBNZ) never matches,
        // exactly the unpatched state. Only the process-wide link
        // generation stays untouched: every other patched edge in the
        // process keeps working.
        {
            bool writable = false;
            for (const usize index : victims) {
                ArenaChunk& chunk = s_arena_chunks[index];
                for (const ArenaIncomingLink& link : chunk.incoming) {
                    if (!arena_code_live(link.gen_site,
                                         link.source_generation)) {
                        continue;  // The source chunk died first.
                    }
                    if (!writable) {
                        pthread_jit_write_protect_np(0);
                        writable = true;
                    }
                    link.gen_site[0] = link.restore_word != 0
                                           ? link.restore_word
                                           : arm64::movz_w(3, 0);
                    sys_icache_invalidate(link.gen_site, sizeof(u32));
                }
                chunk.incoming.clear();
            }
            if (writable) pthread_jit_write_protect_np(1);
        }
        // Step 3: retirement round bookkeeping. The sequence bump is what
        // makes every thread purge at its next dispatcher entry; the epoch
        // bump is what the sweep compares occupancy against.
        s_arena_retire_seq.fetch_add(1, std::memory_order_acq_rel);
        const u64 epoch =
            s_arena_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
        const u64 now = steady_now_ns();
        for (const usize index : victims) {
            ArenaChunk& chunk = s_arena_chunks[index];
            chunk.pending_free = true;
            chunk.retired_epoch = epoch;
            chunk.retired_ns = now;
            chunk.owned = false;
        }
        s_arena_pending_count.fetch_add(victims.size(),
                                        std::memory_order_relaxed);
        s_arena_retire_generation.store(
            s_arena_generation.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        // Age the survivors so "recent" hits mean recent.
        for (usize i = 0; i < s_arena_chunk_count; ++i) {
            ArenaChunk& chunk = s_arena_chunks[i];
            chunk.hits.store(chunk.hits.load(std::memory_order_relaxed) / 2,
                             std::memory_order_relaxed);
        }
        arena_refresh_min_generation_locked();
        arena_recount_locked();
        live_after = arena_live_bytes_locked();
        // Reclaimer half of the Dekker handshake against
        // ArenaReadLease::acquire() / refresh_epoch(): a thread whose slot
        // a later scan misses is one that published after this fence and
        // therefore sees the generation, sequence and epoch stores above.
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }
    // Step 4: the memo must not hand a retired translation to any thread.
    // Records are purged by chunk, not by generation, so a record inserted
    // concurrently under the old generation (a compile that was mid-flight)
    // is caught too; finalize_code() additionally refuses to insert once
    // the chunk generation no longer matches.
    if (s_retirement_observer != nullptr) s_retirement_observer(victims);
    const u64 total = s_arena_reclaims.fetch_add(
        victims.size(), std::memory_order_relaxed) + victims.size();
    if (total <= 16 || (total % 64) == 0) {
        std::fprintf(stderr,
                     "[jit] arena retired %zu chunk(s) (total=%llu pending=%zu "
                     "available=%zu frees=%llu live=%zuMB soft=%zuMB)\n",
                     victims.size(), static_cast<unsigned long long>(total),
                     s_arena_pending_count.load(std::memory_order_relaxed),
                     s_arena_available.load(std::memory_order_relaxed),
                     static_cast<unsigned long long>(
                         s_arena_chunk_frees.load(std::memory_order_relaxed)),
                     live_after / (1024 * 1024),
                     arena_effective_soft_budget() / (1024 * 1024));
    }
    return victims.size();
}

usize JitCodeArena::sweep_retired_chunks(bool drained) {
    if (s_arena_region == nullptr ||
        s_arena_pending_count.load(std::memory_order_acquire) == 0) {
        return 0;
    }
    std::unique_lock<std::mutex> lock(s_arena_chunk_mutex, std::defer_lock);
    if (drained) {
        // The gate round must not come back empty-handed because a chunk
        // take happened to hold the mutex; nothing under it blocks.
        lock.lock();
    } else {
        // Opportunistic: called at every cold dispatcher entry while
        // anything is pending. The scan reads every occupancy line, so
        // bound the rate, and never queue behind a taker.
        static std::atomic<u64> s_last_sweep_ns{0};
        constexpr u64 kMinSweepIntervalNs = 1000ull * 1000ull;  // 1 ms
        const u64 now = steady_now_ns();
        const u64 last = s_last_sweep_ns.load(std::memory_order_relaxed);
        if (now - last < kMinSweepIntervalNs) return 0;
        if (!lock.try_lock()) {
            return 0;  // Another thread is sweeping or taking; it will free.
        }
        s_last_sweep_ns.store(now, std::memory_order_relaxed);
    }
    // Reclaimer half of the handshake: the pending chunks' generation and
    // epoch stores precede this fence; a thread that published its slot
    // after it sees them. A thread the scan does see is treated exactly.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    const u64 oldest_inside = drained ? ~u64{0} : arena_oldest_inside_epoch();
    usize freed = 0;
    for (usize i = 0; i < s_arena_chunk_count; ++i) {
        const ArenaChunk& chunk = s_arena_chunks[i];
        if (!chunk.pending_free) continue;
        // Every thread inside entered at or after the retirement's epoch
        // bump, so none can have this chunk's code in flight.
        if (oldest_inside >= chunk.retired_epoch) {
            arena_free_chunk_locked(i);
            ++freed;
        }
    }
    if (freed != 0) JitCodeArena::arena_recount_locked();
    return freed;
}

void JitCodeArena::abandon_gate_if_stalled() noexcept {
    // How long native execution may stay refused for one rollover attempt.
    // Long enough that a guest which does reach a quiescent point still
    // rolls over; short enough that one which never does barely notices.
    constexpr u64 kMaxGateClosedNs = 100ull * 1000ull * 1000ull;      // 100ms
    constexpr u64 kBaseCooldownNs  = 1000ull * 1000ull * 1000ull;     // 1s
    constexpr u64 kMaxCooldownNs   = 30ull * 1000ull * 1000ull * 1000ull;

    const u64 closed_ns = s_arena_gate_closed_ns.load(std::memory_order_acquire);
    const u64 now = steady_now_ns();
    if (closed_ns == 0 || now - closed_ns < kMaxGateClosedNs) {
        return;
    }

    // Back off further each time draining fails again, so a guest that can
    // never quiesce settles at a negligible duty cycle instead of spending a
    // tenth of every second refusing native entry.
    const u32 failures =
        s_arena_drain_failures.fetch_add(1, std::memory_order_relaxed) + 1;
    u64 cooldown = kBaseCooldownNs;
    for (u32 i = 1; i < failures && cooldown < kMaxCooldownNs; ++i) {
        cooldown *= 2;
    }
    if (cooldown > kMaxCooldownNs) {
        cooldown = kMaxCooldownNs;
    }
    s_arena_reclaim_retry_ns.store(now + cooldown, std::memory_order_release);
    s_arena_gate_closed_ns.store(0, std::memory_order_release);
    const u64 abandons =
        s_arena_gate_abandons.fetch_add(1, std::memory_order_relaxed) + 1;
    // Reopen last: no thread may enter the arena until the bookkeeping above
    // is published.
    s_arena_reclaim_requested.store(false, std::memory_order_release);
    if (abandons <= 4 || (abandons % 32) == 0) {
        std::fprintf(stderr,
                     "[jit] arena reclaim could not drain in %llums; gate "
                     "reopened (abandons=%llu retry_in=%llums refused=%llu)\n",
                     static_cast<unsigned long long>(kMaxGateClosedNs / 1000000ull),
                     static_cast<unsigned long long>(abandons),
                     static_cast<unsigned long long>(cooldown / 1000000ull),
                     static_cast<unsigned long long>(
                         s_arena_gate_refusals.load(std::memory_order_relaxed)));
    }
}

bool JitCodeArena::reclaim_arena_if_requested() {
    if (!s_arena_reclaim_requested.load(std::memory_order_acquire)) {
        // Normal path: no gate. Recycle whatever retired chunks the epoch
        // rule now permits. Nonblocking and only when something is pending.
        (void)sweep_retired_chunks();
        return false;
    }

    // Gate fallback. Host calls are safepoints (HostCallSafepoint), so a
    // thread parked in a guest wait holds no lease; what remains are chains
    // between native entries and compiles in flight. Even so, do not park
    // dispatchers behind a reclaimer: interpreted guest progress may be what
    // lets a lease's owner return. Keep the native-entry gate closed and
    // retry at a later safe point instead of introducing a process-wide wait
    // cycle.
    std::unique_lock<std::mutex> reclaim_lock(s_arena_reclaim_mutex,
                                             std::try_to_lock);
    if (!reclaim_lock.owns_lock()) {
        return false;
    }
    if (!s_arena_reclaim_requested.load(std::memory_order_acquire)) {
        return false;
    }
    if (s_arena_region == nullptr) {
        s_arena_reclaim_requested.store(false, std::memory_order_release);
        return false;
    }

    // Threads continue interpreted while the gate is closed. Scan once per
    // attempt; never wait here for a reader that might need guest progress.
    const auto drain_start = std::chrono::steady_clock::now();
    // The flag is already published (request_arena_reclaim). This fence is
    // the reclaimer half of the Dekker handshake in ArenaReadLease::acquire():
    // every thread that entered the arena without seeing the flag has a slot
    // this scan observes as occupied.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (!arena_leases_drained()) {
        abandon_gate_if_stalled();
        return false;
    }
    const long long drain_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - drain_start).count();
    // The guest does reach quiescent points; drop any backoff a previous
    // stalled attempt left behind.
    JitCodeArena::s_arena_drain_failures.store(0, std::memory_order_relaxed);
    JitCodeArena::s_arena_reclaim_retry_ns.store(0, std::memory_order_release);

    // An explicit request with nothing pending means "make room now": retire
    // the oldest live chunk, even a compiler's current one (it takes another
    // at its next allocation). With chunks already pending, freeing them is
    // the room.
    usize retired = 0;
    if (JitCodeArena::s_arena_pending_count.load(std::memory_order_acquire) == 0) {
        retired = JitCodeArena::retire_arena_chunks(1, /*force=*/true);
    }
    // Nobody is inside: everything pending can go now.
    const usize freed = JitCodeArena::sweep_retired_chunks(/*drained=*/true);

    const u64 closed_ns = JitCodeArena::s_arena_gate_closed_ns.load(std::memory_order_acquire);
    const long long gate_ms =
        closed_ns == 0
            ? -1
            : static_cast<long long>((steady_now_ns() - closed_ns) / 1000000ull);
    JitCodeArena::s_arena_gate_closed_ns.store(0, std::memory_order_release);
    JitCodeArena::s_arena_reclaim_requested.store(false, std::memory_order_release);
    static std::atomic<u64> s_gate_rounds{0};
    const u64 rounds = s_gate_rounds.fetch_add(1, std::memory_order_relaxed) + 1;
    if (rounds <= 16 || (rounds % 64) == 0) {
        std::fprintf(stderr,
                     "[jit] arena gate round %llu: retired=%zu freed=%zu "
                     "drain=%lldus gate=%lldms refused=%llu\n",
                     static_cast<unsigned long long>(rounds), retired, freed,
                     drain_us, gate_ms,
                     static_cast<unsigned long long>(
                         JitCodeArena::s_arena_gate_refusals.load(std::memory_order_relaxed)));
    }
    return true;
}

void* JitCodeArena::alloc(Cursor& cursor, usize size, u64& generation) {
    const usize aligned_size = (size + JitCodeArena::kCodeAlign - 1) & ~(JitCodeArena::kCodeAlign - 1);
    if (JitCodeArena::s_arena_region == nullptr ||
        JitCodeArena::s_arena_growth_failed.load(std::memory_order_acquire) ||
        aligned_size > JitCodeArena::chunk_code_capacity()) {
        return nullptr;
    }
    // Two rounds: the current chunk, then one freshly taken chunk. Nothing
    // here is shared with other compilers except the chunk take itself.
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (cursor.chunk >= 0) {
            JitCodeArena::ArenaChunk& chunk = JitCodeArena::s_arena_chunks[static_cast<usize>(cursor.chunk)];
            // Retired under us (a forced retirement takes owned chunks too):
            // abandon it. A block already emitted into it would fail
            // JitCodeArena::arena_code_live() and never be entered.
            if (chunk.generation.load(std::memory_order_acquire) ==
                cursor.generation) {
                if (cursor.used + aligned_size <= JitCodeArena::chunk_code_capacity()) {
                    void* const mem =
                        static_cast<u8*>(JitCodeArena::s_arena_region) +
                        static_cast<usize>(cursor.chunk) * JitCodeArena::s_arena_chunk_size +
                        cursor.used;
                    cursor.used += aligned_size;
                    chunk.used.store(cursor.used, std::memory_order_relaxed);
                    JitCodeArena::s_arena_used_total.fetch_add(aligned_size,
                                                 std::memory_order_relaxed);
                    generation = cursor.generation;
                    return mem;
                }
            }
        }
        if (!take_chunk(cursor)) {
            return nullptr;  // Deferred; a safe-point sweep makes room.
        }
    }
    return nullptr;
}

bool JitCodeArena::alloc_split(Cursor& cursor, usize hot_size,
                               usize cold_size, void*& hot, void*& cold,
                               u64& generation) {
    const auto align = [](usize n) {
        return (n + JitCodeArena::kCodeAlign - 1) & ~(JitCodeArena::kCodeAlign - 1);
    };
    const usize h = align(hot_size);
    const usize c = align(cold_size);
    if (JitCodeArena::s_arena_region == nullptr ||
        JitCodeArena::s_arena_growth_failed.load(std::memory_order_acquire) ||
        !split_fits(hot_size, cold_size) ||
        kSplitWindow > JitCodeArena::chunk_code_capacity()) {
        return false;
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (cursor.chunk >= 0) {
            JitCodeArena::ArenaChunk& chunk =
                JitCodeArena::s_arena_chunks[static_cast<usize>(cursor.chunk)];
            if (chunk.generation.load(std::memory_order_acquire) ==
                cursor.generation) {
                const bool fits_open = cursor.win_cold != 0 &&
                                       cursor.win_hot + h + c <= cursor.win_cold;
                if (!fits_open &&
                    cursor.used + kSplitWindow <= JitCodeArena::chunk_code_capacity()) {
                    // A new window. The gap left in the old one is the
                    // fragmentation cost: at most one block per window.
                    cursor.win_hot = cursor.used;
                    cursor.win_cold = cursor.used + kSplitWindow;
                    cursor.used += kSplitWindow;
                    chunk.used.store(cursor.used, std::memory_order_relaxed);
                    JitCodeArena::s_arena_used_total.fetch_add(
                        kSplitWindow, std::memory_order_relaxed);
                }
                if (cursor.win_cold != 0 &&
                    cursor.win_hot + h + c <= cursor.win_cold) {
                    u8* const base = static_cast<u8*>(JitCodeArena::s_arena_region) +
                        static_cast<usize>(cursor.chunk) * JitCodeArena::s_arena_chunk_size;
                    hot = base + cursor.win_hot;
                    cursor.win_hot += h;
                    cursor.win_cold -= c;
                    cold = base + cursor.win_cold;
                    generation = cursor.generation;
                    return true;
                }
            }
        }
        if (!take_chunk(cursor)) {
            return false;  // Deferred; a safe-point sweep makes room.
        }
    }
    return false;
}

} // namespace vitreo::cpu::jit
