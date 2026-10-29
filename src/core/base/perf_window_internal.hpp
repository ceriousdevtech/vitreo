// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file perf_window_internal.hpp
/// @brief The perf window's shared State and the small helpers every
///        perf_window*.cpp file uses: perf_window.cpp (runtime and
///        compile-time recorders),
///        perf_window_control.cpp (start/stop/reset, env and test control)
///        and perf_window_report*.cpp (the text report and the snapshot).
///        Everything is in the named namespace perf_window_detail, never an
///        anonymous one: state() is an external inline function, so the
///        process keeps exactly ONE State. state() is also hidden, like the
///        internal-linkage original, so the recorders still address its
///        static and guard directly instead of through the GOT. Include from
///        those files only. Split out of perf_window.cpp as verbatim code
///        motion.

#include <vitreo/diagnostics.hpp>

#include "perf_window.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

namespace vitreo::diagnostics::perf_window_detail {

using Clock = std::chrono::steady_clock;

constexpr u32 kExecBucketCount = 5;
constexpr u32 kSizeBucketCount = 4;
constexpr u32 kWaitKindCount = 4;
constexpr u32 kThunkKindCount = 5;
constexpr u32 kFsKindCount = 3;
constexpr usize kMaxCompileRecs = 131072;
constexpr usize kMaxThreads = 256;

enum class StartAt : u8 {
    Disabled = 0,
    ProcessStart,
    PeEntry,
    FirstWindow,
    FirstDevice,
    FirstDraw,
    FirstPresent,
    Manual,
    /// VITREO_PERF_WINDOW=trigger: the window runs while a marker file
    /// exists (VITREO_PERF_WINDOW_TRIGGER, default /tmp/vitreo-perf-window)
    /// and is reported when it is removed. Brackets an arbitrary stretch of a
    /// long session -- an app's steady state, not its load.
    Trigger,
};

struct CompileRec {
    u64 guest_addr{0};
    u32 guest_insns{0};
    u32 guest_bytes{0};
    u32 native_bytes{0};
    u64 compile_ns{0};
};

struct ThreadAcc {
    std::atomic<u64> jit{0};
    std::atomic<u64> interp{0};
    std::atomic<u64> fallback{0};
    u32 serial{0};
};

struct WaitAcc {
    std::atomic<u64> calls{0};
    std::atomic<u64> ns{0};
    std::atomic<u64> max_ns{0};
};

struct FsAcc {
    std::atomic<u64> calls{0};
    std::atomic<u64> ns{0};
    std::atomic<u64> bytes{0};
};

struct State {
    std::atomic<int> enabled_latch{-1};  // -1 unknown, 0 off, 1 on
    std::atomic<u8> start_at{static_cast<u8>(StartAt::Disabled)};
    std::atomic<bool> dumped{false};
    std::atomic<u64> start_ns{0};
    std::atomic<u64> stop_ns{0};

    std::atomic<u64> jit_insns{0};
    std::atomic<u64> interp_insns{0};
    std::atomic<u64> fallback_insns{0};

    std::atomic<u64> compile_attempts{0};
    std::atomic<u64> compile_successes{0};
    std::atomic<u64> compile_failures{0};
    std::atomic<u64> compile_memo_hits{0};
    std::atomic<u64> compile_ns{0};
    std::atomic<u64> compile_ns_max{0};
    std::atomic<u64> native_bytes{0};
    std::atomic<u64> compile_size_ns[kSizeBucketCount]{};
    std::atomic<u64> compile_size_count[kSizeBucketCount]{};

    std::atomic<u64> dispatcher_entries{0};
    std::atomic<u64> linked_hops{0};
    std::atomic<u64> hop_limit_fallbacks{0};

    // ---- BEGIN dispatcher exit-reason diagnostic (self-contained block) ----
    // Why generated code returned to JitCompiler::execute(). One bucket is
    // bumped per dispatcher entry, so `jit_exit_reason` sums to
    // `dispatcher_entries`. See cpu::jit::JitExitReason.
    std::atomic<u64> jit_exit_reason[kPerfJitExitReasonCount]{};
    /// JitExecResult of the entries that recorded no reason. Cross-tab for
    /// the Unattributed bucket only.
    std::atomic<u64> jit_exit_unattr_result[kPerfJitExecResultCount]{};
    static constexpr u32 kTerminalOpcodes = 2048;
    std::atomic<u64> jit_exit_unattr_terminal[kTerminalOpcodes]{};
    /// Subset of the Unattributed entries with a pending guest exception.
    std::atomic<u64> jit_exit_unattr_raise{0};
    /// Linked-tail return-predictor hits (misses are the RetTail*/IndTail*
    /// exit-reason buckets).
    std::atomic<u64> ret_sticky0_hits{0};
    std::atomic<u64> ret_sticky1_hits{0};
    std::atomic<u64> ret_rsb_hits{0};
    std::atomic<u64> ret_l1_hits{0};
    std::atomic<u64> ind_l1_hits{0};
    // ---- END dispatcher exit-reason diagnostic -----------------------------

    std::atomic<u64> linear_mem_emits{0};
    std::atomic<u64> helper_mem_emits{0};

    std::atomic<u64> rep_movs_calls{0};
    std::atomic<u64> rep_movs_bytes{0};
    std::atomic<u64> rep_stos_calls{0};
    std::atomic<u64> rep_stos_bytes{0};
    std::atomic<u64> fused_memcpy_calls{0};
    std::atomic<u64> fused_memcpy_bytes{0};
    std::atomic<u64> fused_memset_calls{0};
    std::atomic<u64> fused_memset_bytes{0};

    WaitAcc wait[kWaitKindCount]{};
    WaitAcc thunk[kThunkKindCount]{};
    FsAcc fs[kFsKindCount]{};

    std::atomic<u64> presents{0};
    std::atomic<u64> draws{0};
    std::atomic<u64> graphics_ns{0};
    std::atomic<u64> registry_calls{0};
    std::atomic<u64> registry_ns{0};

    std::atomic<u32> guest_threads{0};
    std::atomic<u32> guest_runnable{0};
    std::atomic<u32> guest_suspended{0};
    std::atomic<u32> host_workers{0};
    std::atomic<u32> guest_threads_max{0};
    std::atomic<u32> host_workers_max{0};

    std::atomic<bool> guest_linear{false};
    std::atomic<bool> jit_linear{false};
    std::atomic<bool> jit_linear64{false};
    std::atomic<bool> no_jitcache{false};

    std::mutex compile_mu;
    std::vector<CompileRec> compiles;

    // ---- BEGIN codegen size-model diagnostic (self-contained block) --------
    // Splits emitted native bytes into a per-block FIXED cost and a
    // PER-GUEST-INSTRUCTION cost:
    //
    //     emitted_bytes(block) ~= FIXED + PER_INSN * guest_instruction_count
    //
    // `compiles` already carries (guest_insns, native_bytes) per compile, so
    // the dump could regress over it directly -- except that it stops
    // recording at kMaxCompileRecs, which silently biases a long run toward
    // whatever was compiled first. These sums are therefore accumulated for
    // EVERY successful compile. They are written under `compile_mu`, which
    // perf_note_compile already takes on exactly this path, so they add no
    // lock and no atomic; when the perf window is off perf_note_compile
    // returns before any of it. Plain u64, not atomics, for that reason.
    //
    // Guarded by compile_mu:
    static constexpr u32 kSizeModelMaxN = 16;
    u64 sm_blocks{0};        ///< successful compiles summed here
    u64 sm_sum_n{0};         ///< sum of guest_insns
    u64 sm_sum_n2{0};        ///< sum of guest_insns^2
    u64 sm_sum_y{0};         ///< sum of native_bytes
    u64 sm_sum_ny{0};        ///< sum of guest_insns * native_bytes
    u64 sm_sum_gbytes{0};    ///< sum of guest_bytes (x86 byte length)
    u64 sm_min_bytes{~0ull}; ///< smallest block ever emitted
    /// Per-guest-instruction-count histogram, index = guest_insns, 1..16.
    /// Index 0 collects the (pathological) zero-instruction compiles.
    u64 sm_hist_count[kSizeModelMaxN + 1]{};
    u64 sm_hist_bytes[kSizeModelMaxN + 1]{};
    /// Same split as compile_size_count/_ns, but carrying emitted bytes and
    /// guest instructions so a bucket mean is well defined.
    u64 sm_bucket_count[kSizeBucketCount]{};
    u64 sm_bucket_bytes[kSizeBucketCount]{};
    u64 sm_bucket_insns[kSizeBucketCount]{};
    // ---- END codegen size-model diagnostic ---------------------------------

    std::mutex thread_mu;
    std::vector<ThreadAcc*> threads;
    std::atomic<u32> next_serial{0};

    std::atomic<u64> cache_l1_hits0{0};
    std::atomic<u64> cache_hash_hits0{0};
    std::atomic<u64> cache_misses0{0};
    std::atomic<u64> cache_evictions0{0};
    std::atomic<u64> cache_invalidations0{0};

    // Helper-write target pages. One guest issues 1.39 G calls into
    // jit_mem_write* while producing zero block invalidations: the
    // self-modifying-code guard fires constantly and never catches
    // self-modifying code. Recording which pages separates a few hot
    // false-positive pages from spread traffic, and those call for opposite
    // fixes. Direct-mapped by page; a collision loses the older page rather
    // than merging two counts into a wrong one.
    static constexpr u32 kWritePageSlots = 4096;
    std::atomic<u64> wpage_addr[kWritePageSlots]{};
    std::atomic<u64> wpage_count[kWritePageSlots]{};
    std::atomic<u64> wpage_total{0};
    std::atomic<u64> wpage_collisions{0};
    // Which check-map reason bits were set on the page a helper write hit.
    // The byte carries independent reasons -- mixed protection, holds code,
    // write-watched -- and they call for completely different fixes, so the
    // histogram is useless without knowing WHICH one sent the store here.
    std::atomic<u64> wreason[8]{};
    // Which of the halfword probe's TWO map entries was nonzero. The emitted
    // LDRH consults map[i] and map[i+1] together, so a store can take the cold
    // path because its NEIGHBOUR page needs the helper while its own page does
    // not. The code calls that case rare; this counts it.
    std::atomic<u64> wprobe[4]{};  // 0 neither, 1 self, 2 neighbour, 3 both
    // Read side. `rcross` splits helper reads by whether the access straddles
    // a 16 KiB host page, which is the one reason the x64 load guard routes to
    // the helper that can be derived without re-deriving the guard.
    std::atomic<u64> rtotal{0};
    std::atomic<u64> rwidth[4]{};
    std::atomic<u64> rcross[2]{};
    // The linear64 load guard's remaining exits, by the check-map byte the
    // emitted code consults. 100% of accesses are emitted linear, so a helper
    // read means the INLINE guard failed at run time -- and this says which
    // reason bit was set on the page when it did.
    std::atomic<u64> rreason[8]{};
    std::atomic<u64> rsite_rip[512]{};
    std::atomic<u64> rsite_n[512]{};
    std::atomic<u64> rsite_sampled{0};
    std::atomic<u64> rsite_unmapped{0};

    // Which GUEST INSTRUCTION issued the store that reached the helper.
    // Sampled, not counted: resolving a host PC to a guest RIP at every one of
    // ~1.9 G helper calls would dominate the workload it is measuring. The
    // stride is reported with the histogram so the shares stay readable.
    static constexpr u64 kWriteSiteStride = 1024;
    static constexpr u32 kWriteSiteSlots = 2048;
    std::atomic<u64> wsite_rip[kWriteSiteSlots]{};
    std::atomic<u64> wsite_count[kWriteSiteSlots]{};
    std::atomic<u64> wsite_sampled{0};
    std::atomic<u64> wsite_unmapped{0};
    // Of the unmapped samples: was the return address inside a published JIT
    // block (its cold stub) or in no block at all? Those are different worlds
    // -- the first is the checked store's own slow path, the second means the
    // caller is not the store fast path.
    std::atomic<u64> wsite_cold{0};      // block-owned, no guest RIP
    std::atomic<u64> wsite_foreign{0};   // not in any published block
    std::atomic<u64> wsite_hostpc[State::kWriteSiteSlots > 0 ? 512 : 512]{};
    std::atomic<u64> wsite_hostpc_n[512]{};
    // One resolved symbol name per distinct foreign PC bucket.
    char wsite_sym[8][160]{};
    std::atomic<u64> wsite_sym_pc[8]{};
    std::atomic<u64> wsite_seq{0};
    std::atomic<u64> wwidth[4]{};   // 1, 2, 4, 8 bytes

    static constexpr u32 kMissSlots = 65536;
    std::atomic<u32> miss_epoch{1};
    std::atomic<u64> miss_seen_rip[kMissSlots]{};
    std::atomic<u32> miss_seen_epoch[kMissSlots]{};
    std::atomic<u64> block_miss_total{0};
    std::atomic<u64> block_miss_unique{0};
    std::atomic<u64> block_miss_repeat{0};
    std::atomic<u64> block_miss_collision{0};
    std::atomic<u64> block_miss_empty{0};
    std::atomic<u64> block_miss_empty_repeat{0};
    std::atomic<u64> block_miss_chain{0};
    std::atomic<u64> chain_miss_mid_block{0};
    std::atomic<u64> chain_miss_thunk{0};
    std::atomic<u64> chain_thunk_inline{0};
    std::atomic<u64> chain_thunk_inline_fast{0};
    std::atomic<u64> chain_thunk_inline_slow{0};
    std::atomic<u64> chain_thunk_inline_direct{0};
    std::atomic<u64> jcc_emit_fused{0};
    std::atomic<u64> jcc_emit_carried{0};
    std::atomic<u64> jcc_emit_helper{0};
    // Which (x86 condition code, incoming NzcvState) pairs still fall to the
    // flag helper. The last big win on this ladder came from exactly this
    // class -- a missing Sub8/Sub16 arm in the predicted walk was worth -30%
    // -- so "9 helper sites" is not actionable without knowing WHICH nine.
    std::atomic<u64> jcc_helper_cc[16][8]{};
    std::atomic<u64> rep_emit_inline{0};
    std::atomic<u64> rep_emit_helper{0};
    std::atomic<u64> cf_emit_known{0};
    std::atomic<u64> cf_emit_nzcv{0};
    std::atomic<u64> cf_emit_dispatch{0};

    /// Top-N chain-miss RIP counts (exact while the RIP stays in a slot).
    static constexpr u32 kTopMissSlots = 64;
    std::atomic<u64> chain_top_rip[kTopMissSlots]{};
    std::atomic<u64> chain_top_count[kTopMissSlots]{};
};

[[gnu::visibility("hidden")]] inline State& state() noexcept {
    static State s;
    return s;
}

inline u64 now_ns() noexcept {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch())
            .count());
}

inline void fetch_max(std::atomic<u64>& slot, u64 value) noexcept {
    u64 cur = slot.load(std::memory_order_relaxed);
    while (value > cur &&
           !slot.compare_exchange_weak(cur, value, std::memory_order_relaxed)) {
    }
}

struct CacheSum {
    u64 l1{0};
    u64 hash{0};
    u64 miss{0};
    u64 evict{0};
    u64 inval{0};
    u64 size{0};
};

inline void cache_sum_fn(u64 l1, u64 hash, u64 miss, u64 evict, u64 inval, u64 size,
                         void* ctx) {
    auto* s = static_cast<CacheSum*>(ctx);
    s->l1 += l1;
    s->hash += hash;
    s->miss += miss;
    s->evict += evict;
    s->inval += inval;
    s->size += size;
}

/// Installed by set_perf_cache_visitors and read by sum_caches. Defined in
/// perf_window_report.cpp.
extern std::atomic<PerfVisitCachesFn> g_visit_caches;

inline CacheSum sum_caches() noexcept {
    CacheSum s;
    if (auto fn = g_visit_caches.load(std::memory_order_relaxed)) {
        fn(&cache_sum_fn, &s);
    }
    return s;
}

/// Formats and writes one whole report. Defined in perf_window_report.cpp;
/// perf_window_dump (perf_window_control.cpp) calls it once per window.
void dump_unlocked(const char* reason) noexcept;

}  // namespace vitreo::diagnostics::perf_window_detail
