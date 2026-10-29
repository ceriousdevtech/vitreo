// SPDX-License-Identifier: LGPL-2.1-only

/// @file perf_window_report.cpp
/// @brief Perf-window report orchestrator (dump_unlocked), the structured
///        snapshot, and the dump-time hooks. dump_unlocked prints the header,
///        guest instructions, JIT compilation and the execution histogram,
///        then calls the perf_window_report_*.cpp sections in their original
///        order. Split out of perf_window.cpp as verbatim code motion.

#include <vitreo/diagnostics.hpp>

#include "perf_window.hpp"
#include "perf_window_report_internal.hpp"

#include <algorithm>
#include <atomic>
#include <dlfcn.h>
#include <cstdarg>
#include <string>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace {
std::atomic<vitreo::diagnostics::PerfVisitCompiledFn> g_visit_compiled{nullptr};
std::atomic<vitreo::diagnostics::PerfThreadSnapshotFn> g_thread_snapshot{nullptr};
}  // namespace

namespace vitreo::diagnostics {
using namespace perf_window_detail;

namespace perf_window_detail {

std::atomic<PerfVisitCachesFn> g_visit_caches{nullptr};

}  // namespace perf_window_detail

namespace {

u32 exec_bucket(u64 execs) noexcept {
    if (execs <= 1) return 0;
    if (execs == 2) return 1;
    if (execs <= 4) return 2;
    if (execs <= 16) return 3;
    return 4;
}

const char* exec_bucket_name(u32 i) noexcept {
    switch (i) {
        case 0: return "1";
        case 1: return "2";
        case 2: return "3-4";
        case 3: return "5-16";
        default: return "17+";
    }
}

}  // namespace

namespace perf_window_detail {

void dump_unlocked(const char* reason) noexcept {
    ReportBuf out;
    // Close the window BEFORE any dump-time work. The thread-snapshot hook
    // below takes ThreadManager's mutex and can block behind a live guest
    // thread, and when stop_ns was never set this function falls back to
    // "now", so anything done first is silently reported as run time rather
    // than as shutdown latency.
    auto& st = state();
    const u64 start = st.start_ns.load(std::memory_order_relaxed);
    u64 stop = st.stop_ns.load(std::memory_order_relaxed);
    if (stop == 0) {
        stop = now_ns();
    }
    const u64 window_ns = (start != 0 && stop >= start) ? (stop - start) : 0;

    if (auto fn = g_thread_snapshot.load(std::memory_order_relaxed)) {
        fn();
    }

    const u64 jit = st.jit_insns.load(std::memory_order_relaxed);
    const u64 interp = st.interp_insns.load(std::memory_order_relaxed);
    const u64 fallback = st.fallback_insns.load(std::memory_order_relaxed);
    const u64 guest = jit + interp + fallback;

    const u64 attempts = st.compile_attempts.load(std::memory_order_relaxed);
    const u64 successes = st.compile_successes.load(std::memory_order_relaxed);
    const u64 failures = st.compile_failures.load(std::memory_order_relaxed);
    const u64 memo = st.compile_memo_hits.load(std::memory_order_relaxed);
    const u64 compile_ns = st.compile_ns.load(std::memory_order_relaxed);
    const u64 compile_max = st.compile_ns_max.load(std::memory_order_relaxed);
    const u64 native_bytes = st.native_bytes.load(std::memory_order_relaxed);

    const u64 disp = st.dispatcher_entries.load(std::memory_order_relaxed);
    const u64 hops = st.linked_hops.load(std::memory_order_relaxed);
    const double link_ratio =
        (disp + hops) ? (100.0 * static_cast<double>(hops) /
                         static_cast<double>(disp + hops))
                      : 0.0;

    const CacheSum cache = sum_caches();
    auto sat_sub = [](u64 a, u64 b) noexcept { return a >= b ? a - b : a; };
    const u64 l1 = sat_sub(
        cache.l1, st.cache_l1_hits0.load(std::memory_order_relaxed));
    const u64 hash = sat_sub(
        cache.hash, st.cache_hash_hits0.load(std::memory_order_relaxed));
    const u64 miss = sat_sub(
        cache.miss, st.cache_misses0.load(std::memory_order_relaxed));
    const u64 evict = sat_sub(
        cache.evict, st.cache_evictions0.load(std::memory_order_relaxed));
    const u64 inval = sat_sub(
        cache.inval, st.cache_invalidations0.load(std::memory_order_relaxed));
    const u64 lookups = l1 + hash + miss;
    const double l1_rate =
        lookups ? (100.0 * static_cast<double>(l1) / static_cast<double>(lookups))
                : 0.0;

    u64 wait_ns = 0;
    u64 wait_calls = 0;
    for (const auto& w : st.wait) {
        wait_ns += w.ns.load(std::memory_order_relaxed);
        wait_calls += w.calls.load(std::memory_order_relaxed);
    }
    const u64 sleep_ns = st.wait[static_cast<u32>(PerfWaitKind::Sleep)].ns.load(
        std::memory_order_relaxed);
    u64 fs_ns = 0;
    u64 fs_calls = 0;
    u64 fs_bytes = 0;
    for (const auto& f : st.fs) {
        fs_ns += f.ns.load(std::memory_order_relaxed);
        fs_calls += f.calls.load(std::memory_order_relaxed);
        fs_bytes += f.bytes.load(std::memory_order_relaxed);
    }

    const u64 helper_r = g_helper_mem.reads.load(std::memory_order_relaxed);
    const u64 helper_w = g_helper_mem.writes.load(std::memory_order_relaxed);
    const u64 linear_emits = st.linear_mem_emits.load(std::memory_order_relaxed);
    const u64 helper_emits = st.helper_mem_emits.load(std::memory_order_relaxed);
    const u64 mem_emits = linear_emits + helper_emits;

    // The pid is part of the header because several guest processes
    // dumps one report per process and they all land in the same log. Without
    // it a reader cannot tell which process a report describes.
    out("\n========== VITREO PERF WINDOW (%s) pid=%d ==========\n",
        reason ? reason : "dump", static_cast<int>(::getpid()));
    out("Window:                  %.3fs\n",
                 static_cast<double>(window_ns) / 1e9);
    out("Guest instructions:      %llu\n",
                 static_cast<unsigned long long>(guest));
    out("  JIT:                   %llu (%.1f%%)\n",
                 static_cast<unsigned long long>(jit),
                 guest ? 100.0 * static_cast<double>(jit) /
                             static_cast<double>(guest)
                       : 0.0);
    out("  Interpreter:           %llu (%.1f%%)\n",
                 static_cast<unsigned long long>(interp),
                 guest ? 100.0 * static_cast<double>(interp) /
                             static_cast<double>(guest)
                       : 0.0);
    out("  Fallback/delegate:     %llu (%.1f%%)\n",
                 static_cast<unsigned long long>(fallback),
                 guest ? 100.0 * static_cast<double>(fallback) /
                             static_cast<double>(guest)
                       : 0.0);

    out("\nJIT compilation:\n");
    out("  Attempts:              %llu\n",
                 static_cast<unsigned long long>(attempts));
    out("  Successes:             %llu\n",
                 static_cast<unsigned long long>(successes));
    out("  Failures:              %llu\n",
                 static_cast<unsigned long long>(failures));
    out("  Memo hits:             %llu\n",
                 static_cast<unsigned long long>(memo));
    out("  Wall time:             %.3fs\n",
                 static_cast<double>(compile_ns) / 1e9);
    out("  Average:               %.1fus\n",
                 successes ? (static_cast<double>(compile_ns) /
                              static_cast<double>(successes) / 1e3)
                           : 0.0);
    out("  Maximum:               %.1fus\n",
                 static_cast<double>(compile_max) / 1e3);
    out("  Native bytes:          %llu\n",
                 static_cast<unsigned long long>(native_bytes));
    out("  By guest block size:\n");
    for (u32 i = 0; i < kSizeBucketCount; ++i) {
        const u64 n = st.compile_size_count[i].load(std::memory_order_relaxed);
        const u64 ns = st.compile_size_ns[i].load(std::memory_order_relaxed);
        out("    %s insns:         %llu  %.3fs\n",
                     size_bucket_name(i), static_cast<unsigned long long>(n),
                     static_cast<double>(ns) / 1e9);
    }

    struct ExecHist {
        u64 blocks[kExecBucketCount]{};
        u64 native_bytes[kExecBucketCount]{};
        u64 compile_ns[kExecBucketCount]{};
        u64 unmatched_ns{0};
        u64 compiled_once{0};
        u64 compiled_le2{0};
        u64 compiled_le4{0};
    } hist;

    struct HistCtx {
        ExecHist* hist;
        const std::vector<CompileRec>* recs;
    };

    std::vector<CompileRec> recs;
    {
        std::lock_guard<std::mutex> lock(st.compile_mu);
        recs = st.compiles;
    }

    auto visit = [](u64 addr, u64 execs, u32 nbytes, u32 /*ginsns*/,
                    void* ctx) {
        auto* h = static_cast<HistCtx*>(ctx);
        const u32 b = exec_bucket(execs == 0 ? 1 : execs);
        h->hist->blocks[b] += 1;
        h->hist->native_bytes[b] += nbytes;
        if (execs <= 1) h->hist->compiled_once += 1;
        if (execs <= 2) h->hist->compiled_le2 += 1;
        if (execs <= 4) h->hist->compiled_le4 += 1;
        for (const auto& rec : *h->recs) {
            if (rec.guest_addr == addr) {
                h->hist->compile_ns[b] += rec.compile_ns;
            }
        }
        (void)addr;
    };
    HistCtx hctx{&hist, &recs};
    if (auto fn = g_visit_compiled.load(std::memory_order_relaxed)) {
        fn(visit, &hctx);
    }

    u64 matched_ns = 0;
    for (u32 i = 0; i < kExecBucketCount; ++i) {
        matched_ns += hist.compile_ns[i];
    }
    hist.unmatched_ns =
        compile_ns > matched_ns ? (compile_ns - matched_ns) : 0;

    out("\nCompiled-block final execution counts (live cache):\n");
    for (u32 i = 0; i < kExecBucketCount; ++i) {
        out(
                     "  %s:                  %llu blocks  %llu native bytes  "
                     "%.3fs compile\n",
                     exec_bucket_name(i),
                     static_cast<unsigned long long>(hist.blocks[i]),
                     static_cast<unsigned long long>(hist.native_bytes[i]),
                     static_cast<double>(hist.compile_ns[i]) / 1e9);
    }
    out("  Compiled-once:         %llu\n",
                 static_cast<unsigned long long>(hist.compiled_once));
    out("  Compiled and exec <=2: %llu\n",
                 static_cast<unsigned long long>(hist.compiled_le2));
    out("  Compiled and exec <=4: %llu\n",
                 static_cast<unsigned long long>(hist.compiled_le4));
    if (hist.unmatched_ns != 0) {
        out("  Evicted/unmatched compile time: %.3fs\n",
                     static_cast<double>(hist.unmatched_ns) / 1e9);
    }

    report_codegen_size_model(out, st);
    report_jit_exit_reasons(out, st, disp);
    report_dispatch_cache(out, st, disp, hops, link_ratio, l1, l1_rate, hash,
                          miss, evict, inval, cache);
    report_memory(out, st, linear_emits, helper_emits, mem_emits, helper_r,
                  helper_w);
    report_runtime(out, st, window_ns, wait_ns, wait_calls, sleep_ns, fs_ns,
                   fs_calls, fs_bytes);

    out(
                 "\nNotes: ARM inline L1 tails are not in C++ L1/hash counts. "
                 "Linked-hop ratio uses dispatcher returns + hop counter. "
                 "Wait time covers blocked APIs; thread snapshot does not "
                 "classify wait-blocked vs running.\n");
    out("==============================================\n");
    out.flush();
}

}  // namespace perf_window_detail

PerfWindowSnapshot perf_window_snapshot() noexcept {
    auto& st = state();
    PerfWindowSnapshot s;
    s.enabled = perf_window_enabled();
    s.active = perf_window_active();
    const u64 start = st.start_ns.load(std::memory_order_relaxed);
    u64 stop = st.stop_ns.load(std::memory_order_relaxed);
    if (s.active) {
        stop = now_ns();
    }
    s.window_ns = (start != 0 && stop >= start) ? (stop - start) : 0;
    s.jit_insns = st.jit_insns.load(std::memory_order_relaxed);
    s.interp_insns = st.interp_insns.load(std::memory_order_relaxed);
    s.fallback_insns = st.fallback_insns.load(std::memory_order_relaxed);
    s.guest_insns = s.jit_insns + s.interp_insns + s.fallback_insns;
    s.compile_attempts = st.compile_attempts.load(std::memory_order_relaxed);
    s.compile_successes = st.compile_successes.load(std::memory_order_relaxed);
    s.compile_failures = st.compile_failures.load(std::memory_order_relaxed);
    s.compile_memo_hits = st.compile_memo_hits.load(std::memory_order_relaxed);
    s.compile_ns = st.compile_ns.load(std::memory_order_relaxed);
    s.compile_ns_max = st.compile_ns_max.load(std::memory_order_relaxed);
    s.native_bytes = st.native_bytes.load(std::memory_order_relaxed);
    s.dispatcher_entries = st.dispatcher_entries.load(std::memory_order_relaxed);
    s.linked_hops = st.linked_hops.load(std::memory_order_relaxed);
    s.hop_limit_fallbacks = st.hop_limit_fallbacks.load(std::memory_order_relaxed);
    s.block_miss_total = st.block_miss_total.load(std::memory_order_relaxed);
    s.block_miss_unique = st.block_miss_unique.load(std::memory_order_relaxed);
    s.block_miss_repeat = st.block_miss_repeat.load(std::memory_order_relaxed);
    s.block_miss_collision = st.block_miss_collision.load(std::memory_order_relaxed);
    s.block_miss_empty = st.block_miss_empty.load(std::memory_order_relaxed);
    s.block_miss_empty_repeat =
        st.block_miss_empty_repeat.load(std::memory_order_relaxed);
    s.block_miss_chain = st.block_miss_chain.load(std::memory_order_relaxed);
    s.chain_miss_mid_block =
        st.chain_miss_mid_block.load(std::memory_order_relaxed);
    s.chain_miss_thunk = st.chain_miss_thunk.load(std::memory_order_relaxed);
    s.chain_thunk_inline =
        st.chain_thunk_inline.load(std::memory_order_relaxed);
    s.chain_thunk_inline_fast =
        st.chain_thunk_inline_fast.load(std::memory_order_relaxed);
    s.chain_thunk_inline_slow =
        st.chain_thunk_inline_slow.load(std::memory_order_relaxed);
    s.chain_thunk_inline_direct =
        st.chain_thunk_inline_direct.load(std::memory_order_relaxed);
    s.jcc_emit_fused = st.jcc_emit_fused.load(std::memory_order_relaxed);
    s.jcc_emit_carried = st.jcc_emit_carried.load(std::memory_order_relaxed);
    s.jcc_emit_helper = st.jcc_emit_helper.load(std::memory_order_relaxed);
    s.rep_emit_inline = st.rep_emit_inline.load(std::memory_order_relaxed);
    s.rep_emit_helper = st.rep_emit_helper.load(std::memory_order_relaxed);
    s.cf_emit_known = st.cf_emit_known.load(std::memory_order_relaxed);
    s.cf_emit_nzcv = st.cf_emit_nzcv.load(std::memory_order_relaxed);
    s.cf_emit_dispatch =
        st.cf_emit_dispatch.load(std::memory_order_relaxed);
    s.helper_reads = g_helper_mem.reads.load(std::memory_order_relaxed);
    s.helper_writes = g_helper_mem.writes.load(std::memory_order_relaxed);
    s.mem_faults = g_helper_mem.faults.load(std::memory_order_relaxed);
    s.linear_mem_emits = st.linear_mem_emits.load(std::memory_order_relaxed);
    s.helper_mem_emits = st.helper_mem_emits.load(std::memory_order_relaxed);
    s.rep_movs_calls = st.rep_movs_calls.load(std::memory_order_relaxed);
    s.rep_movs_bytes = st.rep_movs_bytes.load(std::memory_order_relaxed);
    s.rep_stos_calls = st.rep_stos_calls.load(std::memory_order_relaxed);
    s.rep_stos_bytes = st.rep_stos_bytes.load(std::memory_order_relaxed);
    s.fused_memcpy_calls = st.fused_memcpy_calls.load(std::memory_order_relaxed);
    s.fused_memcpy_bytes = st.fused_memcpy_bytes.load(std::memory_order_relaxed);
    s.fused_memset_calls = st.fused_memset_calls.load(std::memory_order_relaxed);
    s.fused_memset_bytes = st.fused_memset_bytes.load(std::memory_order_relaxed);
    for (const auto& w : st.wait) {
        s.wait_ns += w.ns.load(std::memory_order_relaxed);
    }
    s.sleep_ns = st.wait[static_cast<u32>(PerfWaitKind::Sleep)].ns.load(
        std::memory_order_relaxed);
    for (const auto& f : st.fs) {
        s.fs_ns += f.ns.load(std::memory_order_relaxed);
    }
    s.registry_calls = st.registry_calls.load(std::memory_order_relaxed);
    s.registry_ns = st.registry_ns.load(std::memory_order_relaxed);
    s.presents = st.presents.load(std::memory_order_relaxed);
    s.draws = st.draws.load(std::memory_order_relaxed);
    s.graphics_ns = st.graphics_ns.load(std::memory_order_relaxed);
    s.guest_threads = st.guest_threads.load(std::memory_order_relaxed);
    s.guest_runnable = st.guest_runnable.load(std::memory_order_relaxed);
    s.guest_suspended = st.guest_suspended.load(std::memory_order_relaxed);
    s.host_workers = st.host_workers.load(std::memory_order_relaxed);
    s.guest_threads_max = st.guest_threads_max.load(std::memory_order_relaxed);
    s.host_workers_max = st.host_workers_max.load(std::memory_order_relaxed);
    s.guest_linear = st.guest_linear.load(std::memory_order_relaxed);
    s.jit_linear = st.jit_linear.load(std::memory_order_relaxed);
    s.jit_linear64 = st.jit_linear64.load(std::memory_order_relaxed);
    s.no_jitcache = st.no_jitcache.load(std::memory_order_relaxed);
    return s;
}

void set_perf_cache_visitors(PerfVisitCachesFn caches,
                             PerfVisitCompiledFn compiled) noexcept {
    g_visit_caches.store(caches, std::memory_order_relaxed);
    g_visit_compiled.store(compiled, std::memory_order_relaxed);
}

void set_perf_thread_snapshot(PerfThreadSnapshotFn fn) noexcept {
    g_thread_snapshot.store(fn, std::memory_order_relaxed);
}

void perf_window_poll_threads() noexcept {
    if (!perf_window_active()) return;
    if (auto fn = g_thread_snapshot.load(std::memory_order_relaxed)) {
        fn();
    }
}

}  // namespace vitreo::diagnostics
