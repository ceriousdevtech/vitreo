// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

/// @file jit_code_arena.hpp
/// @brief The process-wide MAP_JIT code arena: one reservation, recycled in
///        chunks, shared by every compiler in the process.
///
/// Extracted from JitCompiler, where it was ~40 static data members and ~30
/// methods among 135 and 285. Its public API sat around line 180 of that
/// header and the state those accessors read sat around line 1280 -- a
/// thousand lines apart, in the same class, with nothing saying they were one
/// thing.
///
/// The distinction that move makes visible: **this state is process-wide and
/// a compiler is not.** Everything here outlives any one JitCompiler and is
/// shared by all of them; what a compiler owns is a `Cursor`, three fields
/// naming the chunk it is currently bump-allocating into. Previously both
/// lived in the same object and nothing marked which was which, so a change
/// to either had to be reasoned about against all 135 members.
///
/// Scope note, because it is a natural question: this arena is already shared
/// across THREADS -- there is no thread_local here, and compiled code is
/// shared between threads through the process-wide compile memo. It is not
/// shared across PROCESSES, and this extraction does not make it so; see the
/// class comment on Cursor for what would be required.

#include <vitreo/common.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <vector>

namespace vitreo::cpu::jit {

/// One MAP_JIT reservation, carved into equal chunks and recycled per chunk.
///
/// All state is static: there is one arena per process, not one per compiler.
class JitCodeArena {
public:
    /// A single compiler's bump cursor into the chunk it has borrowed.
    ///
    /// This is the only per-compiler part of the arena, and separating it is
    /// the point of the type: a compiler holds a Cursor, the process holds the
    /// arena.
    ///
    /// Why the arena is not shared across processes, since the code below
    /// makes it look close: emitted code is already position-independent for
    /// almost everything -- per-thread state is reached through pinned X19
    /// (CpuState) and X20 (JitContext), and nearly every helper call is
    /// `LDR X16,[X20,#off]; BLR X16`. Three things still bind a compiled block
    /// to one process, and all three are enumerable rather than vague:
    ///
    ///   1. 49 emit sites bake an absolute host function address as a
    ///      MOVZ/MOVK run into X16 (this is why the emission golden test
    ///      resolves those through dladdr rather than hashing them).
    ///   2. Interpreter-delegate stencils bake `&instructions[i]`, a heap
    ///      pointer.
    ///   3. Direct-link tails patch MOV immediates holding arena target
    ///      addresses, and the arena base differs per process.
    ///
    /// Plus the host constraint: MAP_JIT is per-process, so cross-process
    /// sharing needs a different mapping strategy under the hardened runtime.
    struct Cursor {
        i32   chunk{-1};
        u64   generation{0};
        usize used{0};
        // Current split window (alloc_split), as chunk offsets: hot code
        // grows up from `win_hot`, cold code down from `win_cold`. win_cold
        // == 0 means no open window. Reset whenever the chunk changes.
        usize win_hot{0};
        usize win_cold{0};
    };

    using RetirementObserver = void (*)(const std::vector<usize>& victims);

    /// Called with the chunk indices being retired, while their code is still
    /// mapped and before their bytes can be reused.
    ///
    /// The arena retires chunks. It does not know that anything caches
    /// translations, and it should not: this hook exists because the retire
    /// path used to reach straight into the compile memo's private state,
    /// which is invisible coupling while both live in one class and a link
    /// error the moment they do not. The memo registers here at startup. One
    /// function pointer, set once, is the whole interface.
    static void set_retirement_observer(RetirementObserver fn) noexcept {
        s_retirement_observer = fn;
    }

    /// Initialise the process-wide arena, once. A compiler asks; the once_flag
    /// and the initialiser stay private.
    static void ensure_initialized();

    /// Record `n` recent dispatcher entries against the chunk holding `code`,
    /// for victim selection. A compiler samples; the chunk table stays here.
    static void note_chunk_hit(const void* code, u32 n) noexcept;

    /// Record that `gen_site` (in the chunk holding `source_code`) was patched
    /// to branch into the chunk holding `target_code`, so retiring the target
    /// can fail that site closed again. Called by the direct-link patcher.
    /// A non-zero `restore_word` is written back instead of the generation
    /// poison: a retargeted mid-block B.cond returns to its exit stub.
    static void arena_note_incoming_link(const void* target_code,
                                         const void* source_code,
                                         u64 source_generation,
                                         u32* gen_site,
                                         u32 restore_word = 0);
    /// Write one code word at `site` if the chunk holding it is still live
    /// at `source_generation`, atomically with respect to retirement and
    /// reuse (both take the chunk mutex). Returns whether it wrote.
    static bool arena_write_word_if_live(u32* site, u64 source_generation,
                                         u32 word);

    /// Bump-allocate `size` bytes in `cursor`'s chunk, taking a new chunk if
    /// the current one cannot fit it. Returns null when the arena is full or
    /// disabled; `generation` receives the owning chunk's generation.
    static void* alloc(Cursor& cursor, usize size, u64& generation);

    /// Window for split (hot/cold) block placement. TBZ/TBNZ reach +-32 KiB,
    /// and a hot and a cold address in one window are always closer than its
    /// size, so every branch between a block's two parts stays encodable.
    static constexpr usize kSplitWindow = 32ull * 1024;

    /// Whether a block's two parts fit one split window together.
    [[nodiscard]] static constexpr bool split_fits(usize hot_size,
                                                   usize cold_size) noexcept {
        const usize mask = kCodeAlign - 1;
        return ((hot_size + mask) & ~mask) + ((cold_size + mask) & ~mask) <=
               kSplitWindow;
    }

    /// Place one block split: `hot_size` bytes growing up from the bottom of
    /// the cursor's current window, `cold_size` bytes growing down from its
    /// top, opening a new window (or chunk) when the two would meet. The hot
    /// parts of consecutive blocks are therefore contiguous, and no block's
    /// cold bytes sit in the hot stream. The cold part is always above the
    /// hot part. False when the pair does not fit one window (split_fits) or
    /// the arena is full -- the same transient, retryable refusal as alloc().
    static bool alloc_split(Cursor& cursor, usize hot_size, usize cold_size,
                            void*& hot, void*& cold, u64& generation);

    /// Bytes of arena code currently allocated in live chunks.
    [[nodiscard]] static usize arena_bytes_used() noexcept {
        return s_arena_used_total.load(std::memory_order_relaxed);
    }
    /// Size of one recyclable arena chunk (the unit of retirement).
    [[nodiscard]] static usize arena_capacity() noexcept {
        return s_arena_chunk_size;
    }
    /// Bytes of one chunk that hold code. The rest, at the top, is the
    /// chunk's far-branch island (alloc_far_branch).
    [[nodiscard]] static usize chunk_code_capacity() noexcept {
        return s_arena_chunk_size - s_arena_island_size;
    }

    /// Reserve `bytes` in the far-branch island of the chunk holding
    /// `source_code`, for a veneer that site can reach with B/BL.
    ///
    /// A direct-link patch is one B/BL imm26 (+-128 MiB), but the arena is
    /// gigabytes and a CALL edge joins code compiled far apart in time, so a
    /// game's call sites into early-compiled callees were mostly out of
    /// range: never patched, they ran their slot's guard and then the L1
    /// probe anyway. The island sits inside the source's own chunk, so it is
    /// always in range, and it lives and dies with that chunk: only that
    /// chunk's sites can reach it. Returns null when `source_code` is not
    /// live arena code at `source_generation` or the island is full.
    static void* alloc_far_branch(const void* source_code,
                                  u64 source_generation, usize bytes);

    /// Whether `p` lies in the far-branch island of the chunk holding
    /// `source_code`.
    [[nodiscard]] static bool in_far_branch_island(const void* source_code,
                                                   const void* p) noexcept {
        if (s_arena_region == nullptr || s_arena_island_size == 0) return false;
        const usize index = arena_chunk_index(source_code);
        if (index >= s_arena_chunk_count) return false;
        const u8* const top = static_cast<const u8*>(s_arena_region) +
                              (index + 1) * s_arena_chunk_size;
        const u8* const q = static_cast<const u8*>(p);
        return q >= top - s_arena_island_size && q < top;
    }
    /// RWX page inside the arena reservation, immediately below chunk 0 and
    /// outside every chunk, for process-wide shared stubs; null before
    /// initialization or when the arena could not be mapped. Apple cores
    /// predict BLR/BR targets near the branch far better than targets
    /// gigabytes away, so hot trampolines want their stubs here.
    static constexpr usize kSharedStubPageSize = 65536;
    [[nodiscard]] static void* shared_stub_page() noexcept {
        return s_arena_stub_page;
    }
    /// Chunks retired so far (their code invalidated; bytes freed once the
    /// epoch condition holds — see arena_chunk_frees()).
    [[nodiscard]] static u64 arena_reclaim_count() noexcept {
        return s_arena_reclaims.load(std::memory_order_relaxed);
    }
    [[nodiscard]] static u64 arena_chunk_retirements() noexcept {
        return s_arena_reclaims.load(std::memory_order_relaxed);
    }
    /// Retired chunks whose bytes have actually been recycled.
    [[nodiscard]] static u64 arena_chunk_frees() noexcept {
        return s_arena_chunk_frees.load(std::memory_order_relaxed);
    }
    /// Retired chunks still waiting for every thread that was inside native
    /// code at their retirement epoch to leave or re-enter.
    [[nodiscard]] static usize arena_pending_free_count() noexcept {
        return s_arena_pending_count.load(std::memory_order_relaxed);
    }
    /// Chunks a compiler could take right now without any retirement.
    [[nodiscard]] static usize arena_available_chunks() noexcept {
        return s_arena_available.load(std::memory_order_relaxed);
    }
    [[nodiscard]] static usize arena_chunk_count() noexcept {
        return s_arena_chunk_count;
    }
    /// Chunks currently holding live code.
    [[nodiscard]] static usize arena_segment_count() noexcept {
        std::lock_guard<std::mutex> lock(s_arena_chunk_mutex);
        usize live = 0;
        for (usize i = 0; i < s_arena_chunk_count; ++i) {
            live += s_arena_chunks[i].generation.load(
                        std::memory_order_relaxed) != 0 ? 1 : 0;
        }
        return live;
    }
    /// Summed capacity ceiling across all chunks (the whole reservation).
    [[nodiscard]] static usize arena_budget() noexcept {
        return s_arena_budget;
    }
    /// Bytes of MAP_JIT currently holding live code (excludes guard pages).
    [[nodiscard]] static usize arena_bytes_mapped() noexcept {
        return arena_segment_count() * s_arena_chunk_size;
    }
    /// Bytes of live (armed, not retired) chunks, whole chunks counted: what
    /// the soft budget caps.
    [[nodiscard]] static usize arena_live_bytes() noexcept {
        std::lock_guard<std::mutex> lock(s_arena_chunk_mutex);
        return arena_live_bytes_locked();
    }
    /// Configured resident soft budget (before pressure adjustment).
    [[nodiscard]] static usize arena_soft_budget() noexcept {
        return s_arena_soft_budget;
    }
    /// The soft budget a retirement round is measured against right now:
    /// the configured one under the current memory pressure, or the budget
    /// this process has learned its live working set needs, whichever is
    /// larger (see "Retirement thrash" below).
    [[nodiscard]] static usize arena_current_soft_budget() noexcept {
        return arena_effective_soft_budget();
    }

    // ---- Retirement thrash ------------------------------------------------
    // The soft budget caps RECLAIMABLE code. Retiring code that is still in
    // the working set reclaims nothing: it is translated again at once, into
    // a fresh chunk, so the footprint stays at the budget while every round
    // costs a round of recompiles. A process whose live translations exceed
    // the budget therefore recompiles without end (a 64-bit title menu,
    // ~890 MB live against 512 MB: 3 FPS against 39).
    //
    // So the arena measures it. Every translation a retirement round takes
    // away is remembered by (guest address, guest code hash) in a small
    // fixed table (note_retired_translation, called by each compiler as it
    // drops the pointers); a fresh emission of the same address AND the same
    // bytes within kThrashWindowRounds rounds is a re-translation
    // (note_fresh_translation), and its bytes are counted. Code the guest
    // regenerated is NOT counted: new code at a new address has no mark, and
    // new bytes at an old address hash differently. When the bytes
    // re-translated since the previous round reach half of the bytes that
    // round retired, kThrashRounds rounds in a row, the round was retiring
    // the working set: this process's soft budget is doubled (up to the
    // whole arena) and the raise is logged. It is never lowered again.
    static constexpr u32 kThrashRounds = 2;
    static constexpr u32 kThrashWindowRounds = 8;
    /// A compiler dropped its pointer to a translation of the guest code at
    /// @p addr whose bytes hash to @p code_hash, because a round retired it.
    static void note_retired_translation(u64 addr, u64 code_hash) noexcept;
    /// A compiler emitted @p bytes of fresh arena code for the guest code at
    /// @p addr hashing to @p code_hash. Counted when that translation was
    /// retired by a recent round.
    static void note_fresh_translation(u64 addr, u64 code_hash,
                                       usize bytes) noexcept;
    /// Bytes counted by note_fresh_translation since the process started.
    [[nodiscard]] static u64 arena_retranslated_bytes() noexcept {
        return s_retranslated_total.load(std::memory_order_relaxed);
    }
    /// Times thrash raised this process's soft budget.
    [[nodiscard]] static u32 arena_soft_budget_raises() noexcept {
        return s_soft_raises.load(std::memory_order_relaxed);
    }
    /// Tests: forget every raise, mark and re-translation count, so the next
    /// test starts from the configured budget.
    static void reset_soft_budget_learning() noexcept;
    /// Oldest generation any live chunk carries (generation + 1 when no chunk
    /// holds code). Code below this has been retired; code at or above it may
    /// still have been retired individually — arena_code_live() is exact.
    [[nodiscard]] static u64 arena_min_generation() noexcept {
        return s_arena_min_generation.load(std::memory_order_acquire);
    }
    [[nodiscard]] static u64 arena_generation() noexcept {
        return s_arena_generation.load(std::memory_order_acquire);
    }
    /// Monotonic count of retirement rounds. A thread that observes a change
    /// must purge every published pointer that no longer satisfies
    /// arena_code_live() before entering native code again.
    [[nodiscard]] static u64 arena_retire_seq() noexcept {
        return s_arena_retire_seq.load(std::memory_order_acquire);
    }
    [[nodiscard]] static u64 arena_epoch() noexcept {
        return s_arena_epoch.load(std::memory_order_acquire);
    }
    /// Lease-publication read of the epoch (the caller fences).
    [[nodiscard]] static u64 arena_epoch_relaxed() noexcept {
        return s_arena_epoch.load(std::memory_order_relaxed);
    }

    /// Index of the chunk holding `code`. Caller guarantees `code` lies in
    /// the reservation.
    [[nodiscard]] static usize arena_chunk_index(const void* code) noexcept {
        return static_cast<usize>(static_cast<const u8*>(code) -
                                  static_cast<const u8*>(s_arena_region)) >>
               s_arena_chunk_shift;
    }
    /// Exact liveness of a published native pointer: the chunk holding it
    /// still carries the generation it was compiled under. One acquire load
    /// and one compare, so it is cheap enough for every dispatcher entry.
    /// Generation 0 never names live code (it marks "not compiled" in
    /// BasicBlock and "holds no code" in a chunk), so it is refused outright.
    [[nodiscard]] static bool arena_code_live(const void* code,
                                              u64 generation) noexcept {
        if (code == nullptr || generation == 0 || s_arena_region == nullptr) {
            return false;
        }
        const usize index = arena_chunk_index(code);
        if (index >= s_arena_chunk_count) return false;
        return s_arena_chunks[index].generation.load(
                   std::memory_order_acquire) == generation;
    }
    /// The generation of the chunk holding @p code, 0 when it holds no live
    /// code or @p code is outside the arena.
    [[nodiscard]] static u64 arena_live_generation(const void* code) noexcept {
        if (code == nullptr || s_arena_region == nullptr) return 0;
        const u8* const p = static_cast<const u8*>(code);
        const u8* const base = static_cast<const u8*>(s_arena_region);
        if (p < base) return 0;
        const usize index = arena_chunk_index(code);
        if (index >= s_arena_chunk_count) return 0;
        return s_arena_chunks[index].generation.load(std::memory_order_acquire);
    }

    /// Roll the shared arena to a fresh immutable segment at the next
    /// dispatcher safe point. The normal producer is arena_alloc() when the
    /// current bump segment fills; public exposure makes the gate and native
    /// pointer lifetime contract directly testable.
    static void request_arena_reclaim() noexcept {
        const u64 now = steady_now_ns();
        // Honour the backoff left by an attempt that could not drain. Without
        // it the very next failed arena_alloc reshuts the gate immediately and
        // the process spends all of its time refusing native entry.
        if (now < s_arena_reclaim_retry_ns.load(std::memory_order_acquire)) {
            return;
        }
        bool expected = false;
        if (s_arena_reclaim_requested.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            s_arena_gate_closed_ns.store(now, std::memory_order_release);
        }
    }

    /// Monotonic nanoseconds, shared by the gate's open/close bookkeeping.
    [[nodiscard]] static u64 steady_now_ns() noexcept {
        return static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    /// Native entries refused, then given back because the drain never
    /// completed within the bounded closed window.
    [[nodiscard]] static u64 arena_gate_abandons() noexcept {
        return s_arena_gate_abandons.load(std::memory_order_relaxed);
    }

    /// Free space below which a chunk is treated as having no room for any
    /// realistic block. Comfortably above an emitted block.
    static constexpr usize kArenaMinUsefulBytes = 64 * 1024;

    /// True when this compiler can land another block without waiting: its
    /// current chunk still has room, another chunk is available, or one can
    /// be taken now (retiring cold chunks if the free pool is low). False
    /// means every chunk is live and none of the retired ones has satisfied
    /// its epoch condition yet; the caller defers the compile.
    [[nodiscard]] static bool has_room(Cursor& cursor) noexcept;

    /// Give this compiler's current chunk back to the pool. The bytes it
    /// already holds stay live; a later compiler resumes bump allocation
    /// after them. With `seal`, the chunk is marked full instead so no
    /// compiler ever allocates from it again (tests use this to force the
    /// next block into a different chunk).
    static void release_chunk(Cursor& cursor, bool seal = false) noexcept;

    /// Retire up to `count` chunks now: their code is invalidated, incoming
    /// direct links are unpatched and their memo records purged; the bytes
    /// are recycled by a later sweep once the epoch condition holds. With
    /// `force`, the oldest live chunks are taken even if a compiler is
    /// allocating into them; otherwise owned chunks are skipped and victims
    /// are the least recently hit, oldest first. Returns chunks retired.
    static usize retire_arena_chunks(usize count, bool force);
    /// Retire exactly the chunk holding `code` if it is live. Tests.
    static bool retire_arena_chunk_containing(const void* code);

    /// Recycle the bytes of every retired chunk that no thread can still be
    /// executing: every thread inside native code at its retirement epoch
    /// has since published a later epoch or left. Nonblocking; returns the
    /// number of chunks freed. `drained` asserts that no thread is inside at
    /// all (the gate path), which frees everything pending.
    static usize sweep_retired_chunks(bool drained = false);

    /// Drop the backoff an undrainable attempt left behind, so the next
    /// request_arena_reclaim() is honoured immediately. A successful rollover
    /// does this on its own; this is for callers that need a reclaim now.
    static void reset_arena_reclaim_backoff() noexcept {
        s_arena_drain_failures.store(0, std::memory_order_relaxed);
        s_arena_reclaim_retry_ns.store(0, std::memory_order_release);
    }
    /// Native entries refused because the arena gate was closed for a
    /// rollover. Every one of them ran interpreted instead.
    [[nodiscard]] static u64 arena_gate_refusals() noexcept {
        return s_arena_gate_refusals.load(std::memory_order_relaxed);
    }
    /// Dispatcher chains must yield their lease even without a GUI pump.
    [[nodiscard]] static bool arena_reclaim_pending() noexcept {
        return s_arena_reclaim_requested.load(std::memory_order_acquire);
    }

    /// At a dispatcher safe point, replace the executable segment after all
    /// active compilers/executors have left it. Published pointers are
    /// invalidated by the arena generation before the old mapping is removed.
    /// Nonblocking: returns false while a reader or another reclaimer is
    /// active, allowing interpreted guest progress to release nested leases.
    /// Returns true for the thread that performed the rollover.
    static bool reclaim_arena_if_requested();

    /// Give the gate back when the bounded closed window expires without the
    /// leases ever draining. New compiles keep failing (the arena is still
    /// full) and so keep interpreting, but every block already compiled runs
    /// natively again instead of the whole process falling to the interpreter.
    /// Caller holds s_arena_reclaim_mutex.
    static void abandon_gate_if_stalled() noexcept;

    // ---- JIT code arena (one MAP_JIT reservation, recycled in chunks) ------
    // The arena is a single MAP_JIT reservation of `budget` bytes, carved into
    // equal chunks (kArenaChunkSize). Each compiler bump-allocates inside its
    // own current chunk — a private add, no shared atomics — and takes a new
    // chunk under s_arena_chunk_mutex only when that one fills. A chunk given
    // back by an exiting compiler keeps its code and is resumed by the next
    // taker, so short-lived guest threads do not each burn a chunk.
    //
    // MAP_JIT pages are dirty private memory: every byte emitted stays resident
    // (or in swap) until its chunk is recycled. The reservation is therefore
    // NOT the footprint bound; the soft budget is (kArenaSoftBudget).
    //
    // Reclamation is per chunk and never stops the world:
    //
    //  1. Retire (retire_arena_chunks): the victim's generation is stored 0,
    //     so arena_code_live() fails for every pointer into it; every direct
    //     link patched INTO it from another chunk is failed closed again
    //     (its MOVZ generation immediate rewritten to 0 — the site's own
    //     generation guard, not the process-wide link generation); its memo
    //     records are purged; s_arena_retire_seq is bumped so each thread
    //     drops its stale L1/sticky/RSB pointers at its next dispatcher entry
    //     (synchronize_arena_generation); and the reclamation epoch is bumped.
    //  2. Sweep (sweep_retired_chunks): the pages are released
    //     (MADV_FREE_REUSABLE) only once no thread still publishes an
    //     occupancy epoch older than that bump — every thread that could have
    //     been executing the chunk's code has since left native execution or
    //     re-entered it (and re-validated everything it holds). Threads that
    //     are parked in a host call hold no occupancy at all. Nobody waits;
    //     the sweep runs at dispatcher safe points and chunk takes.
    //
    // Victims are chosen when the live chunks would pass the soft budget, or
    // when the free pool runs low: least recently hit
    // first, oldest generation as the tie-break, never a chunk a compiler is
    // currently allocating into unless nothing else is left. Hot blocks that
    // lose their chunk are simply recompiled into the current one (they are
    // already past the hot threshold), so hot code migrates forward and cold
    // code ages out.
    //
    // The old drain gate (request_arena_reclaim / reclaim_arena_if_requested)
    // survives as the fallback for the pathological case: a thread that never
    // leaves native code and so keeps retired chunks pending. It closes native
    // entry briefly, frees everything pending once every lease is released,
    // and backs off if that never happens.
    //
    // Two properties are deliberate and were each paid for by a measured
    // failure:
    //
    //  - ONE reservation, registered with the signal handler's JIT code-range
    //    table exactly once. That table publishes an immutable snapshot per
    //    change and never reuses one (a handler may still hold an old pointer),
    //    so its pool is a hard per-process budget. An arena that mapped and
    //    unmapped a segment per rollover exhausted it and then failed every
    //    registration, silently disabling native compilation process-wide.
    //    Recycling a chunk with MAP_FIXED touches no range bookkeeping.
    //
    //  - Retirement is incremental. A single-slot arena can only reclaim by
    //    discarding EVERY translation at once, which also empties the
    //    process-wide compile memo; retiring one chunk costs that chunk's
    //    translations and the rest of the memo survives.
    //
    // History. 512 MB ring slots with a full-drain rollover: MEASURED on a
    // launcher (2026-08-25) a 512 MB budget cost nothing but budgets of 256 MB and
    // below did not pay for themselves (recompilation volume, not mechanism).
    // 2026-09-04: budget raised to 4 GB because a 64-bit title fills 512 MB two
    // minutes into its load and its ~60 parked guest threads never let the
    // drain complete, so every later block ran interpreted. The chunk model
    // replaces the drain entirely; the budget stays a virtual reservation
    // whose pages cost memory only once code lands in them.
    // 2026-10-18: nothing retired until ~3.7 GB of it was dirty. A browser
    // renderer whose JavaScript engine keeps regenerating code held ~690 MB
    // of arena pages (504 MB swapped) after 90 s against ~80 MB of live
    // translations; ten such processes made the host swap. Hence the soft
    // budget below.
    static constexpr usize kArenaSize = 4096ull * 1024 * 1024; // total budget
    // Resident cap on live chunks (VITREO_JIT_ARENA_SOFT_MB overrides; 0
    // disables), raised per process by retirement thrash (above). Before a fresh chunk would pass it, the coldest chunks are
    // retired down to soft - max(soft/8, one chunk), so a round is paid once
    // per ~soft/8 of new code rather than once per chunk. 512 MB is the budget
    // measured cost-free on a launcher (above) — and that measurement was under
    // the full-drain ring, which discarded every translation and the memo at
    // once; per-chunk coldest-first retirement is strictly cheaper. Halved
    // under host memory-pressure warn, quartered under critical, never below
    // kArenaSoftBudgetFloor -- and never below the budget thrash taught the
    // process. 2026-10-19: a 64-bit title menu keeps ~890 MB of live
    // translations; at a flat 512 MB every round retired the working set and
    // the title ran at 3 FPS instead of 39. The 512 MB cost-nothing figure
    // was measured on a launcher.
    static constexpr usize kArenaSoftBudget = 512ull * 1024 * 1024;
    static constexpr usize kArenaSoftBudgetFloor = 64ull * 1024 * 1024;
    // The reservation is carved into chunks of this size (power of two,
    // `VITREO_JIT_ARENA_CHUNK_MB` overrides, 1-512). A chunk is the unit of
    // retirement: retiring one invalidates only the code it holds, unpatches
    // only the direct links that enter it, and purges only its memo records.
    // 8 MB is a few thousand blocks — small enough that a retirement costs a
    // fraction of a percent of a large guest's working set, large enough
    // that taking a new one (the only global allocator operation) is rare.
    static constexpr usize kArenaChunkSize = 8ull * 1024 * 1024;
    static constexpr usize kCodeAlign = 16;

    /// A direct-link patch site in another chunk that branches into this
    /// one. Recorded when the site is patched so retirement can fail it
    /// closed again (MOVZ generation immediate back to 0) without touching
    /// the process-wide link generation.
    struct ArenaIncomingLink {
        u32* gen_site{nullptr};
        u64  source_generation{0};
        u32  restore_word{0};  ///< 0: write the MOVZ W3,#0 poison
    };
    /// Per-chunk metadata. `generation`, `hits` and `used` are read on hot
    /// paths without the mutex; everything else is touched only under
    /// s_arena_chunk_mutex.
    struct ArenaChunk {
        // Generation the chunk was armed with; 0 while it holds no live
        // code (virgin, retired, or freed). Stored 0 FIRST at retirement so
        // arena_code_live() fails before anything else changes.
        std::atomic<u64> generation{0};
        // Recent-hit estimate (sampled in execute(), halved every
        // retirement round) for victim selection.
        std::atomic<u64> hits{0};
        // Bytes bump-allocated so far; written by the owning compiler.
        std::atomic<usize> used{0};
        // A compiler is allocating into it (its private bump chunk).
        bool owned{false};
        // Retired at `retired_epoch`; bytes not yet recycled.
        bool pending_free{false};
        u64 retired_epoch{0};
        u64 retired_ns{0};
        std::vector<ArenaIncomingLink> incoming;
        // Bytes handed out from the top of the chunk by alloc_far_branch.
        usize island_used{0};
        // Pages released with MADV_FREE_REUSABLE; the kernel may discard
        // them at any time until MADV_FREE_REUSE, which must precede reuse.
        bool reusable{false};
    };

    static RetirementObserver s_retirement_observer;
    static std::once_flag   s_arena_init_flag;
    static void*            s_arena_reservation; // Reservation + guards
    static usize            s_arena_total;       // Reservation size
    static void*            s_arena_stub_page;   // Below chunk 0
    static void*            s_arena_region;      // Chunk 0 (code region base)
    static usize            s_arena_budget;      // Code region size
    static usize            s_arena_chunk_size;  // Bytes per chunk (pow2)
    static usize            s_arena_island_size; // Far-branch island per chunk
    static u32              s_arena_chunk_shift; // log2(s_arena_chunk_size)
    static usize            s_arena_chunk_count;
    static usize            s_arena_soft_budget; // Configured resident cap
    static std::unique_ptr<ArenaChunk[]> s_arena_chunks;
    // Guards chunk metadata and the take/retire/sweep transitions. Cold:
    // taken once per chunk, never per block.
    static std::mutex       s_arena_chunk_mutex;
    // Chunks a compiler can take without retiring anything.
    static std::atomic<usize> s_arena_available;
    // Retired chunks whose bytes are not yet recycled.
    static std::atomic<usize> s_arena_pending_count;
    // Bytes allocated in live chunks (diagnostics).
    static std::atomic<usize> s_arena_used_total;
    // Reclamation epoch. Every thread inside the arena publishes the epoch
    // it entered at (see ArenaReadLease); a retirement bumps it, and a
    // retired chunk is recycled only once no thread still publishes an
    // epoch older than the bump — i.e. every thread that could have been
    // executing its code has left or re-entered since.
    static std::atomic<u64> s_arena_epoch;
    // Retirement rounds so far; the per-thread purge trigger.
    static std::atomic<u64> s_arena_retire_seq;
    static std::atomic<u64> s_arena_chunk_frees;
    // Arena generation at the last retirement round; chunks armed after it
    // are ranked last as victims. Written under s_arena_chunk_mutex.
    static std::atomic<u64> s_arena_retire_generation;
    // Compilers and dispatch chains enter a lock-free arena epoch. Segment
    // rollover closes the gate and waits for active emitters/executors before
    // invalidating pointers and unmapping the previous code.
    // A shared_mutex here is prohibitively expensive: once a writer waits,
    // every short JIT block contends in the host kernel.
    // Nested leases on one thread share a single occupancy publication so a
    // dispatch chain does not touch shared state on every block.
    // Occupancy itself is SHARDED per thread (see constructor_destructor.cpp):
    // one shared atomic counter made acquire()/release() a cross-core RMW on a
    // single cacheline, measured at 14.7% of a 32-thread guest's non-idle CPU.
    public:
    class ArenaReadLease {
    public:
        struct DeferAcquire {};
        struct StableExecution {};

        ArenaReadLease() noexcept;
        explicit ArenaReadLease(StableExecution) noexcept;
        explicit ArenaReadLease(DeferAcquire) noexcept;
        ~ArenaReadLease();
        ArenaReadLease(const ArenaReadLease&) = delete;
        ArenaReadLease& operator=(const ArenaReadLease&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept {
            return m_acquired;
        }
        [[nodiscard]] bool acquire() noexcept;
        void release() noexcept;
        [[nodiscard]] static bool thread_holds() noexcept;
        /// The calling thread's lease state, for callers that run on one
        /// thread for their whole life and are entered per dispatched block.
        /// On Darwin EVERY thread_local access -- initial-exec or not -- is a
        /// `_tlv_get_addr` call, and thread_holds() + refresh_epoch() were
        /// two of them on every JitCompiler::execute() (1.1% of a browser
        /// renderer's busy time was that function). Resolve the address
        /// once, then use the `*_at` overloads below. The pointer is valid
        /// for the life of the thread and must not be used from another.
        [[nodiscard]] static void* thread_state() noexcept;
        [[nodiscard]] static bool thread_holds_at(const void* state) noexcept;
        static void refresh_epoch_at(void* state) noexcept;
        /// Re-publish this thread's occupancy at the current epoch. Only
        /// valid at a point where the thread holds no pointer into the arena
        /// that a retirement could have invalidated without it noticing —
        /// the dispatcher entry, where every published pointer is about to
        /// be re-validated. Lets a long-lived chain lease stop blocking the
        /// recycling of chunks it can no longer reach.
        static void refresh_epoch() noexcept;

    private:
        bool m_acquired{false};
    };

    /// A host call made from compiled code is a safepoint.
    ///
    /// A thread inside a Win32 handler is not executing arena code, so it must
    /// not hold a lease there. Reclamation needs every lease released at one
    /// instant, and a guest that parks dozens of threads in waits never
    /// provides one while a parked thread keeps its dispatch-chain lease:
    /// A 64-bit title's inline fast thunks (Sleep, WaitForSingleObject,
    /// EnterCriticalSection, the SRW acquires, SleepConditionVariableCS) all
    /// block with the chain lease held, so the drain never completed and every
    /// block compiled after the arena filled ran interpreted.
    ///
    /// park() gives the lease back for the host call; unpark() takes it back
    /// and says whether the chain may continue into the compiled continuation
    /// it was about to enter:
    ///
    ///   Continue    lease held again and nothing was retired meanwhile.
    ///   Redispatch  lease held again, but a slot was retired while the thread
    ///               was out. A compiled pointer cached across the call may
    ///               now address recycled bytes: leave the chain and re-enter
    ///               through the normal block lookup at the guest RIP.
    ///   LeaseLost   the gate is closed for a rollover; no native entry.
    ///
    /// Return-path cost is the lease's own re-acquire plus one relaxed load
    /// and compare. Relaxed is enough: a completed retirement publishes the
    /// horizon before it reopens the gate, and the re-acquire's acquire-load
    /// of that gate is what admitted this thread, so the new horizon is
    /// visible to it. A retirement still in flight keeps the gate closed and
    /// is reported as LeaseLost instead.
    ///
    /// Parking is nesting-aware in exactly the way release() is: it retracts
    /// this thread's occupancy only when this lease is the outermost one.
    class HostCallSafepoint {
    public:
        enum class Resume : u8 { Continue, Redispatch, LeaseLost };

        explicit HostCallSafepoint(ArenaReadLease* lease) noexcept
            : m_lease(lease) {}
        HostCallSafepoint(const HostCallSafepoint&) = delete;
        HostCallSafepoint& operator=(const HostCallSafepoint&) = delete;

        /// Give the lease back for the duration of a host call. A null or
        /// unheld lease has nothing to park. Idempotent.
        void park() noexcept {
            if (m_parked || m_lease == nullptr || !*m_lease) {
                return;
            }
            // The retirement sequence this chain last purged against. A
            // retirement that lands while the thread is parked bumps it.
            m_horizon = s_arena_retire_seq.load(std::memory_order_relaxed);
            m_lease->release();
            m_parked = true;
        }

        /// Take the lease back after the host call. Idempotent; the result
        /// is retained in resume().
        Resume unpark() noexcept {
            if (!m_parked) {
                return m_resume;
            }
            m_parked = false;
            if (!m_lease->acquire()) {
                m_resume = Resume::LeaseLost;
                return m_resume;
            }
            m_resume = s_arena_retire_seq.load(std::memory_order_relaxed)
                               == m_horizon
                           ? Resume::Continue
                           : Resume::Redispatch;
            return m_resume;
        }

        [[nodiscard]] Resume resume() const noexcept { return m_resume; }
        [[nodiscard]] bool parked() const noexcept { return m_parked; }

    private:
        ArenaReadLease* m_lease;
        u64 m_horizon{0};
        Resume m_resume{Resume::Continue};
        bool m_parked{false};
    };
    private:
    static std::mutex        s_arena_reclaim_mutex;
    static std::atomic<u64>  s_arena_generation;
    // Oldest generation any live chunk carries. Code below it is retired;
    // arena_code_live() is the exact per-pointer test.
    static std::atomic<u64>  s_arena_min_generation;
    static std::atomic<u64>  s_arena_reclaims;
    static std::atomic<bool> s_arena_reclaim_requested;
    static std::atomic<bool> s_arena_growth_failed;
    // Diagnostics for the gate-closed window: when it shut, and how many
    // native entries it turned into interpreter runs.
    static std::atomic<u64>  s_arena_gate_closed_ns;
    static std::atomic<u64>  s_arena_gate_refusals;
    // Host calls are safepoints (HostCallSafepoint): a thread parked in a
    // guest wait holds no lease, so the drain normally completes. This is the
    // safety net for a lease that still cannot drain — a thread that never
    // returns to a dispatcher safe point — so that a transient rollover
    // interlock can never become a permanent, process-wide loss of native
    // execution. The closed window is bounded: an attempt that cannot drain
    // gives the gate back and retries after a cooldown that backs off while
    // draining keeps failing.
    static std::atomic<u64>  s_arena_reclaim_retry_ns;
    static std::atomic<u32>  s_arena_drain_failures;
    static std::atomic<u64>  s_arena_gate_abandons;

    static void init_shared_arena();
    /// Map the single whole-budget MAP_JIT reservation and register it once
    /// with the signal handler's code-range table.
    static bool map_arena_reservation(usize budget);
    /// Chunk size, clamped to the budget. `VITREO_JIT_ARENA_CHUNK_MB`
    /// overrides it; smaller chunks retire in finer steps at the cost of
    /// more frequent chunk takes.
    static usize arena_chunk_size_config() noexcept;
    /// Give this compiler a chunk to allocate into. Prefers a partially
    /// filled chunk another compiler gave back, then a free one (retiring
    /// cold chunks when the free pool runs low). Caller holds no arena
    /// lock. False when nothing can be taken right now.
    static bool take_chunk(Cursor& cursor);
    /// Retire the given live chunks (the body of retire_arena_chunks).
    static usize retire_arena_victims(std::vector<usize> victims);
    /// Under s_arena_chunk_mutex: recount s_arena_available.
    static void arena_recount_locked() noexcept;
    /// Under s_arena_chunk_mutex: recompute s_arena_min_generation.
    static void arena_refresh_min_generation_locked() noexcept;
    /// Under s_arena_chunk_mutex: recycle one pending chunk's bytes.
    static void arena_free_chunk_locked(usize index) noexcept;
    /// Under s_arena_chunk_mutex: retirement candidates, best victim first.
    static std::vector<usize> arena_victim_order_locked(bool force);
    static usize arena_live_bytes_locked() noexcept;
    /// The soft budget after the host memory-pressure adjustment, never
    /// below the budget thrash taught this process (s_soft_learned).
    static usize arena_effective_soft_budget() noexcept;
    /// Under s_arena_chunk_mutex, once per retirement round, before the next
    /// one is chosen: did the previous round retire the working set? Raises
    /// s_soft_learned and returns true when kThrashRounds rounds in a row did.
    static bool arena_learn_from_thrash_locked();

    // Retirement-thrash state (see "Retirement thrash" above).
    // Marks table: kRetiredMarkSlots words of (24-bit key fingerprint << 8 |
    // round tag 1..255), 0 = empty. Mapped on the first retirement of an
    // arena with a soft budget, never freed (a handler-free, lock-free
    // table: a lost or overwritten mark only under-counts).
    static constexpr u32 kRetiredMarkBits = 19;
    static std::atomic<std::atomic<u32>*> s_retired_marks;
    // Bytes re-translated since the last retirement round (reset by it).
    static std::atomic<u64> s_retranslated_bytes;
    static std::atomic<u64> s_retranslated_total;
    // Under s_arena_chunk_mutex: bytes the last round retired, the round
    // sequence last evaluated, and the consecutive thrashing rounds.
    static u64 s_round_retired_bytes;
    static u64 s_thrash_evaluated_seq;
    static u32 s_thrash_streak;
    // The budget thrash taught this process; 0 until the first raise.
    static std::atomic<usize> s_soft_learned;
    static std::atomic<u32> s_soft_raises;

};

}  // namespace vitreo::cpu::jit
