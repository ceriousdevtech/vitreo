// SPDX-License-Identifier: LGPL-2.1-only

/// @file perf_window_control.cpp
/// @brief Perf-window lifecycle: VITREO_PERF_WINDOW parsing and the trigger
///        poller, start/stop/reset, load-phase starts, the dump entry point,
///        and the test controls. Split out of perf_window.cpp as verbatim
///        code motion.

#include <vitreo/diagnostics.hpp>

#include "perf_window.hpp"
#include "perf_window_internal.hpp"

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

namespace vitreo::diagnostics {
using namespace perf_window_detail;

namespace {

StartAt parse_start_at(const char* value) noexcept {
    if (value == nullptr || value[0] == '\0' ||
        (value[0] == '0' && value[1] == '\0')) {
        return StartAt::Disabled;
    }
    const std::string_view v{value};
    if (v == "1" || v == "process_start") return StartAt::ProcessStart;
    if (v == "pe_entry") return StartAt::PeEntry;
    if (v == "first_window") return StartAt::FirstWindow;
    if (v == "first_device") return StartAt::FirstDevice;
    if (v == "first_draw") return StartAt::FirstDraw;
    if (v == "first_present") return StartAt::FirstPresent;
    if (v == "trigger") return StartAt::Trigger;
    return StartAt::ProcessStart;
}

StartAt start_at_from_phase(LoadPhase phase) noexcept {
    switch (phase) {
        case LoadPhase::ProcessStart: return StartAt::ProcessStart;
        case LoadPhase::PeEntry: return StartAt::PeEntry;
        case LoadPhase::FirstWindow: return StartAt::FirstWindow;
        case LoadPhase::FirstDevice: return StartAt::FirstDevice;
        case LoadPhase::FirstDraw: return StartAt::FirstDraw;
        case LoadPhase::FirstPresent: return StartAt::FirstPresent;
        default: return StartAt::Disabled;
    }
}

void snapshot_cache_origin() noexcept {
    auto& st = state();
    const CacheSum s = sum_caches();
    st.cache_l1_hits0.store(s.l1, std::memory_order_relaxed);
    st.cache_hash_hits0.store(s.hash, std::memory_order_relaxed);
    st.cache_misses0.store(s.miss, std::memory_order_relaxed);
    st.cache_evictions0.store(s.evict, std::memory_order_relaxed);
    st.cache_invalidations0.store(s.inval, std::memory_order_relaxed);
}

void zero_atomics() noexcept {
    auto& st = state();
    st.jit_insns.store(0, std::memory_order_relaxed);
    st.interp_insns.store(0, std::memory_order_relaxed);
    st.fallback_insns.store(0, std::memory_order_relaxed);
    st.compile_attempts.store(0, std::memory_order_relaxed);
    st.compile_successes.store(0, std::memory_order_relaxed);
    st.compile_failures.store(0, std::memory_order_relaxed);
    st.compile_memo_hits.store(0, std::memory_order_relaxed);
    st.compile_ns.store(0, std::memory_order_relaxed);
    st.compile_ns_max.store(0, std::memory_order_relaxed);
    st.native_bytes.store(0, std::memory_order_relaxed);
    for (u32 i = 0; i < kSizeBucketCount; ++i) {
        st.compile_size_ns[i].store(0, std::memory_order_relaxed);
        st.compile_size_count[i].store(0, std::memory_order_relaxed);
    }
    st.dispatcher_entries.store(0, std::memory_order_relaxed);
    st.linked_hops.store(0, std::memory_order_relaxed);
    st.hop_limit_fallbacks.store(0, std::memory_order_relaxed);
    // ---- BEGIN dispatcher exit-reason diagnostic reset ----
    for (u32 i = 0; i < kPerfJitExitReasonCount; ++i) {
        st.jit_exit_reason[i].store(0, std::memory_order_relaxed);
    }
    for (u32 i = 0; i < kPerfJitExecResultCount; ++i) {
        st.jit_exit_unattr_result[i].store(0, std::memory_order_relaxed);
    }
    for (u32 i = 0; i < State::kTerminalOpcodes; ++i) {
        st.jit_exit_unattr_terminal[i].store(0, std::memory_order_relaxed);
    }
    st.jit_exit_unattr_raise.store(0, std::memory_order_relaxed);
    st.ret_sticky0_hits.store(0, std::memory_order_relaxed);
    st.ret_sticky1_hits.store(0, std::memory_order_relaxed);
    st.ret_rsb_hits.store(0, std::memory_order_relaxed);
    st.ret_l1_hits.store(0, std::memory_order_relaxed);
    st.ind_l1_hits.store(0, std::memory_order_relaxed);
    // ---- END dispatcher exit-reason diagnostic reset ----
    st.block_miss_total.store(0, std::memory_order_relaxed);
    st.block_miss_unique.store(0, std::memory_order_relaxed);
    st.block_miss_repeat.store(0, std::memory_order_relaxed);
    st.block_miss_collision.store(0, std::memory_order_relaxed);
    st.block_miss_empty.store(0, std::memory_order_relaxed);
    st.block_miss_empty_repeat.store(0, std::memory_order_relaxed);
    st.block_miss_chain.store(0, std::memory_order_relaxed);
    st.chain_miss_mid_block.store(0, std::memory_order_relaxed);
    st.chain_miss_thunk.store(0, std::memory_order_relaxed);
    st.chain_thunk_inline.store(0, std::memory_order_relaxed);
    st.chain_thunk_inline_fast.store(0, std::memory_order_relaxed);
    st.chain_thunk_inline_slow.store(0, std::memory_order_relaxed);
    st.chain_thunk_inline_direct.store(0, std::memory_order_relaxed);
    for (auto& t : st.thunk) {
        t.calls.store(0, std::memory_order_relaxed);
        t.ns.store(0, std::memory_order_relaxed);
        t.max_ns.store(0, std::memory_order_relaxed);
    }
    st.jcc_emit_fused.store(0, std::memory_order_relaxed);
    st.jcc_emit_carried.store(0, std::memory_order_relaxed);
    st.jcc_emit_helper.store(0, std::memory_order_relaxed);
    for (u32 c = 0; c < 16; ++c)
        for (u32 k = 0; k < 8; ++k)
            st.jcc_helper_cc[c][k].store(0, std::memory_order_relaxed);
    st.rep_emit_inline.store(0, std::memory_order_relaxed);
    st.rep_emit_helper.store(0, std::memory_order_relaxed);
    st.cf_emit_known.store(0, std::memory_order_relaxed);
    st.cf_emit_nzcv.store(0, std::memory_order_relaxed);
    st.cf_emit_dispatch.store(0, std::memory_order_relaxed);
    for (u32 i = 0; i < State::kTopMissSlots; ++i) {
        st.chain_top_rip[i].store(0, std::memory_order_relaxed);
        st.chain_top_count[i].store(0, std::memory_order_relaxed);
    }
    {
        u32 epoch = st.miss_epoch.load(std::memory_order_relaxed) + 1u;
        if (epoch == 0) {
            epoch = 1;
            for (u32 i = 0; i < 8; ++i) {
                st.wreason[i].store(0, std::memory_order_relaxed);
            }
            for (u32 i = 0; i < 4; ++i) {
                st.wwidth[i].store(0, std::memory_order_relaxed);
                st.wprobe[i].store(0, std::memory_order_relaxed);
                st.rwidth[i].store(0, std::memory_order_relaxed);
            }
            st.rtotal.store(0, std::memory_order_relaxed);
            st.rcross[0].store(0, std::memory_order_relaxed);
            st.rcross[1].store(0, std::memory_order_relaxed);
            for (u32 i = 0; i < 8; ++i) {
                st.rreason[i].store(0, std::memory_order_relaxed);
            }
            st.rsite_sampled.store(0, std::memory_order_relaxed);
            st.rsite_unmapped.store(0, std::memory_order_relaxed);
            for (u32 i = 0; i < 512; ++i) {
                st.rsite_rip[i].store(0, std::memory_order_relaxed);
                st.rsite_n[i].store(0, std::memory_order_relaxed);
            }
            for (u32 i = 0; i < State::kWriteSiteSlots; ++i) {
                st.wsite_rip[i].store(0, std::memory_order_relaxed);
                st.wsite_count[i].store(0, std::memory_order_relaxed);
            }
            st.wsite_sampled.store(0, std::memory_order_relaxed);
            st.wsite_unmapped.store(0, std::memory_order_relaxed);
            st.wsite_seq.store(0, std::memory_order_relaxed);
            for (u32 i = 0; i < State::kWritePageSlots; ++i) {
                st.wpage_addr[i].store(0, std::memory_order_relaxed);
                st.wpage_count[i].store(0, std::memory_order_relaxed);
            }
            st.wpage_total.store(0, std::memory_order_relaxed);
            st.wpage_collisions.store(0, std::memory_order_relaxed);
            for (u32 i = 0; i < State::kMissSlots; ++i) {
                st.miss_seen_epoch[i].store(0, std::memory_order_relaxed);
            }
        }
        st.miss_epoch.store(epoch, std::memory_order_relaxed);
    }
    g_helper_mem.reads.store(0, std::memory_order_relaxed);
    g_helper_mem.writes.store(0, std::memory_order_relaxed);
    g_helper_mem.faults.store(0, std::memory_order_relaxed);
    st.linear_mem_emits.store(0, std::memory_order_relaxed);
    st.helper_mem_emits.store(0, std::memory_order_relaxed);
    st.rep_movs_calls.store(0, std::memory_order_relaxed);
    st.rep_movs_bytes.store(0, std::memory_order_relaxed);
    st.rep_stos_calls.store(0, std::memory_order_relaxed);
    st.rep_stos_bytes.store(0, std::memory_order_relaxed);
    st.fused_memcpy_calls.store(0, std::memory_order_relaxed);
    st.fused_memcpy_bytes.store(0, std::memory_order_relaxed);
    st.fused_memset_calls.store(0, std::memory_order_relaxed);
    st.fused_memset_bytes.store(0, std::memory_order_relaxed);
    for (auto& w : st.wait) {
        w.calls.store(0, std::memory_order_relaxed);
        w.ns.store(0, std::memory_order_relaxed);
        w.max_ns.store(0, std::memory_order_relaxed);
    }
    for (auto& f : st.fs) {
        f.calls.store(0, std::memory_order_relaxed);
        f.ns.store(0, std::memory_order_relaxed);
        f.bytes.store(0, std::memory_order_relaxed);
    }
    st.presents.store(0, std::memory_order_relaxed);
    st.draws.store(0, std::memory_order_relaxed);
    st.graphics_ns.store(0, std::memory_order_relaxed);
    st.registry_calls.store(0, std::memory_order_relaxed);
    st.registry_ns.store(0, std::memory_order_relaxed);
    st.guest_threads.store(0, std::memory_order_relaxed);
    st.guest_runnable.store(0, std::memory_order_relaxed);
    st.guest_suspended.store(0, std::memory_order_relaxed);
    st.host_workers.store(0, std::memory_order_relaxed);
    st.guest_threads_max.store(0, std::memory_order_relaxed);
    st.host_workers_max.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(st.compile_mu);
        st.compiles.clear();
        // ---- BEGIN codegen size-model diagnostic reset ----
        st.sm_blocks = 0;
        st.sm_sum_n = 0;
        st.sm_sum_n2 = 0;
        st.sm_sum_y = 0;
        st.sm_sum_ny = 0;
        st.sm_sum_gbytes = 0;
        st.sm_min_bytes = ~0ull;
        for (u32 i = 0; i <= State::kSizeModelMaxN; ++i) {
            st.sm_hist_count[i] = 0;
            st.sm_hist_bytes[i] = 0;
        }
        for (u32 i = 0; i < kSizeBucketCount; ++i) {
            st.sm_bucket_count[i] = 0;
            st.sm_bucket_bytes[i] = 0;
            st.sm_bucket_insns[i] = 0;
        }
        // ---- END codegen size-model diagnostic reset ----
    }
    {
        std::lock_guard<std::mutex> lock(st.thread_mu);
        for (ThreadAcc* t : st.threads) {
            t->jit.store(0, std::memory_order_relaxed);
            t->interp.store(0, std::memory_order_relaxed);
            t->fallback.store(0, std::memory_order_relaxed);
        }
    }
}

void atexit_dump() noexcept {
    if (!perf_window_enabled()) {
        return;
    }
    if (perf_window_active()) {
        perf_window_stop();
    }
    perf_window_dump("atexit");
}

void ensure_atexit() noexcept {
    static std::once_flag once;
    std::call_once(once, [] { std::atexit(atexit_dump); });
}

void parse_env_once() noexcept {
    auto& st = state();
    const int seen = st.enabled_latch.load(std::memory_order_relaxed);
    if (seen >= 0) {
        return;
    }
    const StartAt at = parse_start_at(std::getenv("VITREO_PERF_WINDOW"));
    const bool on = at != StartAt::Disabled;
    st.start_at.store(static_cast<u8>(at), std::memory_order_relaxed);
    if (on) {
        ensure_atexit();
    }
    int expected = -1;
    if (!st.enabled_latch.compare_exchange_strong(
            expected, on ? 1 : 0, std::memory_order_relaxed)) {
        return;
    }
    if (at == StartAt::Trigger) {
        const char* env = std::getenv("VITREO_PERF_WINDOW_TRIGGER");
        std::string path = (env != nullptr && env[0] != '\0')
                               ? std::string(env)
                               : std::string("/tmp/vitreo-perf-window");
        // A poller, not a hook on any hot path: the window's own counters are
        // what is being measured. 200 ms is far below any window worth taking.
        std::thread([path = std::move(path)] {
            bool running = false;
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                const bool present = ::access(path.c_str(), F_OK) == 0;
                if (!running && present) {
                    perf_window_start();
                    running = true;
                } else if (running && !present) {
                    perf_window_stop();
                    perf_window_dump("trigger");
                    return;
                }
            }
        }).detach();
    }
}

}  // namespace

bool perf_window_enabled() noexcept {
    parse_env_once();
    return state().enabled_latch.load(std::memory_order_relaxed) == 1;
}

void perf_window_reset() noexcept {
    auto& st = state();
    g_active.store(false, std::memory_order_relaxed);
    st.dumped.store(false, std::memory_order_relaxed);
    st.start_ns.store(0, std::memory_order_relaxed);
    st.stop_ns.store(0, std::memory_order_relaxed);
    zero_atomics();
}

void perf_window_start() noexcept {
    auto& st = state();
    if (!perf_window_enabled()) {
        return;
    }
    bool expected = false;
    if (!g_active.compare_exchange_strong(expected, true,
                                           std::memory_order_relaxed)) {
        return;
    }
    st.dumped.store(false, std::memory_order_relaxed);
    st.stop_ns.store(0, std::memory_order_relaxed);
    zero_atomics();
    snapshot_cache_origin();
    st.start_ns.store(now_ns(), std::memory_order_relaxed);
    std::fprintf(stderr, "[perf-window] start\n");
}

static std::atomic<void (*)()> g_stop_hook{nullptr};

void perf_window_stop() noexcept {
    auto& st = state();
    bool expected = true;
    if (!g_active.compare_exchange_strong(expected, false,
                                           std::memory_order_relaxed)) {
        return;
    }
    st.stop_ns.store(now_ns(), std::memory_order_relaxed);
    std::fprintf(stderr, "[perf-window] stop +%.0f ms\n",
                 static_cast<double>(st.stop_ns.load(std::memory_order_relaxed) -
                                     st.start_ns.load(std::memory_order_relaxed)) /
                     1e6);
    if (auto* hook = g_stop_hook.load(std::memory_order_acquire)) hook();
}

void perf_set_stop_hook(void (*hook)()) noexcept {
    g_stop_hook.store(hook, std::memory_order_release);
}

void perf_window_dump(const char* reason) noexcept {
    if (!perf_window_enabled() &&
        state().enabled_latch.load(std::memory_order_relaxed) != 1) {
        return;
    }
    bool expected = false;
    if (!state().dumped.compare_exchange_strong(expected, true,
                                                std::memory_order_relaxed)) {
        return;
    }
    dump_unlocked(reason);
}

void perf_window_on_phase(LoadPhase phase) noexcept {
    if (!perf_window_enabled()) {
        return;
    }
    const auto want =
        static_cast<StartAt>(state().start_at.load(std::memory_order_relaxed));
    if (want == StartAt::Manual || want == StartAt::Disabled) {
        return;
    }
    if (start_at_from_phase(phase) == want) {
        perf_window_start();
    }
}

void perf_window_enable_for_tests() noexcept {
    auto& st = state();
    st.start_at.store(static_cast<u8>(StartAt::Manual),
                      std::memory_order_relaxed);
    st.enabled_latch.store(1, std::memory_order_relaxed);
    st.dumped.store(false, std::memory_order_relaxed);
}

void perf_window_enable_for_tests(LoadPhase start_at) noexcept {
    auto& st = state();
    st.start_at.store(static_cast<u8>(start_at_from_phase(start_at)),
                      std::memory_order_relaxed);
    st.enabled_latch.store(1, std::memory_order_relaxed);
    st.dumped.store(false, std::memory_order_relaxed);
}

void perf_window_reset_for_tests() noexcept {
    auto& st = state();
    st.enabled_latch.store(0, std::memory_order_relaxed);
    st.start_at.store(static_cast<u8>(StartAt::Disabled),
                      std::memory_order_relaxed);
    perf_window_reset();
}

}  // namespace vitreo::diagnostics
