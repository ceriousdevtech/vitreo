// SPDX-License-Identifier: LGPL-2.1-only

/// @file perf_window.cpp
/// @brief Opt-in reset/start/stop performance window. See diagnostics.hpp.
///        This file keeps every recorder that tests the window gate, the
///        runtime ones (per dispatch, per helper memory access, per API
///        call) and the compile-time ones (per compiled block or emitted
///        lowering), beside perf_window_active() and the helpers they
///        inline; the rest is in perf_window_control.cpp and
///        perf_window_report*.cpp.

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

namespace perf_window_detail {

// Namespace-scope and constant-initialized so the hot-path accessors in
// perf_window.hpp inline to a plain relaxed load with no static-init guard.
std::atomic<bool> g_active{false};
HelperMemCounters g_helper_mem{};

}  // namespace perf_window_detail

using perf_window_detail::g_active;
using perf_window_detail::g_helper_mem;

namespace {

thread_local ThreadAcc* t_thread = nullptr;

u32 size_bucket(u32 guest_insns) noexcept {
    if (guest_insns <= 4) return 0;
    if (guest_insns <= 16) return 1;
    if (guest_insns <= 64) return 2;
    return 3;
}

ThreadAcc& thread_acc() noexcept {
    if (t_thread != nullptr) {
        return *t_thread;
    }
    auto* acc = new ThreadAcc();
    acc->serial = state().next_serial.fetch_add(1, std::memory_order_relaxed) + 1;
    t_thread = acc;
    std::lock_guard<std::mutex> lock(state().thread_mu);
    if (state().threads.size() < kMaxThreads) {
        state().threads.push_back(acc);
    }
    return *acc;
}

// Returns true once per kWriteSiteStride calls: the sampling gate, kept here
// so the caller pays one relaxed increment when it is not this call's turn.
bool write_site_sample_due() noexcept {
    auto& st = state();
    const u64 n = st.wsite_seq.fetch_add(1, std::memory_order_relaxed);
    return (n % State::kWriteSiteStride) == 0;
}

void note_helper_write_site(u64 guest_rip, u64 host_pc, int kind) noexcept {
    auto& st = state();
    st.wsite_sampled.fetch_add(1, std::memory_order_relaxed);
    if (guest_rip == 0) {
        st.wsite_unmapped.fetch_add(1, std::memory_order_relaxed);
        if (kind == 1) st.wsite_cold.fetch_add(1, std::memory_order_relaxed);
        else if (kind == 2) st.wsite_foreign.fetch_add(1, std::memory_order_relaxed);
        // Histogram the raw host PC instead: a hot arena address is one store
        // site, and the block dump can name it afterwards.
        const u32 hs = static_cast<u32>(
            (host_pc ^ (host_pc >> 9) ^ (host_pc >> 19)) & 511u);
        const u64 seen = st.wsite_hostpc[hs].load(std::memory_order_relaxed);
        if (seen == host_pc) {
            st.wsite_hostpc_n[hs].fetch_add(1, std::memory_order_relaxed);
        } else if (seen == 0) {
            st.wsite_hostpc[hs].store(host_pc, std::memory_order_relaxed);
            st.wsite_hostpc_n[hs].store(1, std::memory_order_relaxed);
        }
        return;
    }
    const u32 slot = static_cast<u32>(
        (guest_rip ^ (guest_rip >> 11) ^ (guest_rip >> 23)) &
        (State::kWriteSiteSlots - 1u));
    const u64 seen = st.wsite_rip[slot].load(std::memory_order_relaxed);
    if (seen == guest_rip) {
        st.wsite_count[slot].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (seen == 0) {
        st.wsite_rip[slot].store(guest_rip, std::memory_order_relaxed);
        st.wsite_count[slot].store(1, std::memory_order_relaxed);
    }
}

void note_helper_write_symbol(u64 host_pc) noexcept {
    auto& st = state();
    for (u32 i = 0; i < 8; ++i) {
        if (st.wsite_sym_pc[i].load(std::memory_order_relaxed) == host_pc) return;
    }
    for (u32 i = 0; i < 8; ++i) {
        u64 expected = 0;
        if (st.wsite_sym_pc[i].compare_exchange_strong(
                expected, host_pc, std::memory_order_relaxed)) {
            Dl_info info{};
            if (dladdr(reinterpret_cast<void*>(static_cast<uintptr_t>(host_pc)),
                       &info) != 0 && info.dli_sname != nullptr) {
                std::snprintf(st.wsite_sym[i], sizeof(st.wsite_sym[i]),
                              "%s +0x%llx", info.dli_sname,
                              static_cast<unsigned long long>(
                                  host_pc - reinterpret_cast<uintptr_t>(
                                                info.dli_saddr)));
            } else {
                std::snprintf(st.wsite_sym[i], sizeof(st.wsite_sym[i]),
                              "<unresolved>");
            }
            return;
        }
    }
}

void note_helper_read(u64 addr, int width) noexcept {
    auto& st = state();
    (void)addr;
    st.rtotal.fetch_add(1, std::memory_order_relaxed);
    const u32 i = (width == 1) ? 0 : (width == 2) ? 1 : (width == 4) ? 2 : 3;
    st.rwidth[i].fetch_add(1, std::memory_order_relaxed);
}

void note_helper_read_reason(u8 bits) noexcept {
    state().rreason[bits & 7u].fetch_add(1, std::memory_order_relaxed);
}

void note_helper_read_cross(bool cross) noexcept {
    state().rcross[cross ? 1 : 0].fetch_add(1, std::memory_order_relaxed);
}

void note_helper_read_site(u64 guest_rip) noexcept {
    auto& st = state();
    st.rsite_sampled.fetch_add(1, std::memory_order_relaxed);
    if (guest_rip == 0) {
        st.rsite_unmapped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const u32 slot = static_cast<u32>(
        (guest_rip ^ (guest_rip >> 11) ^ (guest_rip >> 23)) & 511u);
    const u64 seen = st.rsite_rip[slot].load(std::memory_order_relaxed);
    if (seen == guest_rip) {
        st.rsite_n[slot].fetch_add(1, std::memory_order_relaxed);
    } else if (seen == 0) {
        st.rsite_rip[slot].store(guest_rip, std::memory_order_relaxed);
        st.rsite_n[slot].store(1, std::memory_order_relaxed);
    }
}

void note_helper_write_width(int width) noexcept {
    const u32 i = (width == 1) ? 0 : (width == 2) ? 1 : (width == 4) ? 2 : 3;
    state().wwidth[i].fetch_add(1, std::memory_order_relaxed);
}

void note_helper_write_reason(u8 bits, u8 next_bits) noexcept {
    auto& st = state();
    st.wreason[bits & 7u].fetch_add(1, std::memory_order_relaxed);
    const u32 k = (bits != 0 ? 1u : 0u) | (next_bits != 0 ? 2u : 0u);
    st.wprobe[k].fetch_add(1, std::memory_order_relaxed);
}

void note_helper_write_page(u64 addr) noexcept {
    auto& st = state();
    const u64 page = addr >> 12;
    const u32 slot = static_cast<u32>((page ^ (page >> 13)) &
                                      (State::kWritePageSlots - 1u));
    st.wpage_total.fetch_add(1, std::memory_order_relaxed);
    const u64 seen = st.wpage_addr[slot].load(std::memory_order_relaxed);
    if (seen == page) {
        st.wpage_count[slot].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (seen == 0) {
        st.wpage_addr[slot].store(page, std::memory_order_relaxed);
        st.wpage_count[slot].store(1, std::memory_order_relaxed);
        return;
    }
    st.wpage_collisions.fetch_add(1, std::memory_order_relaxed);
}

void note_miss_rip(u64 rip, bool empty_decode, bool from_chain) noexcept {
    if (!perf_window_active()) return;
    auto& st = state();
    st.block_miss_total.fetch_add(1, std::memory_order_relaxed);
    if (from_chain) {
        st.block_miss_chain.fetch_add(1, std::memory_order_relaxed);
    }
    if (empty_decode) {
        st.block_miss_empty.fetch_add(1, std::memory_order_relaxed);
    }

    const u32 epoch = st.miss_epoch.load(std::memory_order_relaxed);
    const u32 slot = static_cast<u32>(
        (rip ^ (rip >> 16) ^ (rip >> 32)) & (State::kMissSlots - 1u));
    const u32 seen_epoch =
        st.miss_seen_epoch[slot].load(std::memory_order_relaxed);
    const u64 seen_rip =
        st.miss_seen_rip[slot].load(std::memory_order_relaxed);
    if (seen_epoch == epoch && seen_rip == rip) {
        st.block_miss_repeat.fetch_add(1, std::memory_order_relaxed);
        if (empty_decode) {
            st.block_miss_empty_repeat.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    if (seen_epoch == epoch && seen_rip != 0 && seen_rip != rip) {
        st.block_miss_collision.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    st.miss_seen_rip[slot].store(rip, std::memory_order_relaxed);
    st.miss_seen_epoch[slot].store(epoch, std::memory_order_relaxed);
    st.block_miss_unique.fetch_add(1, std::memory_order_relaxed);
}

/// Exact per-RIP counts for chain misses that stay in the 64-slot top table.
/// Match → increment; empty → claim; full → displace lowest-count slot.
void note_chain_top_miss(u64 rip) noexcept {
    auto& st = state();
    for (u32 i = 0; i < State::kTopMissSlots; ++i) {
        if (st.chain_top_rip[i].load(std::memory_order_relaxed) == rip) {
            st.chain_top_count[i].fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    for (u32 i = 0; i < State::kTopMissSlots; ++i) {
        u64 expected = 0;
        if (st.chain_top_rip[i].compare_exchange_strong(
                expected, rip, std::memory_order_relaxed)) {
            st.chain_top_count[i].store(1, std::memory_order_relaxed);
            return;
        }
        if (expected == rip) {
            st.chain_top_count[i].fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    u32 victim = 0;
    u64 victim_count = st.chain_top_count[0].load(std::memory_order_relaxed);
    for (u32 i = 1; i < State::kTopMissSlots; ++i) {
        const u64 c = st.chain_top_count[i].load(std::memory_order_relaxed);
        if (c < victim_count) {
            victim = i;
            victim_count = c;
        }
    }
    u64 old_rip = st.chain_top_rip[victim].load(std::memory_order_relaxed);
    if (old_rip == rip) {
        st.chain_top_count[victim].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (st.chain_top_rip[victim].compare_exchange_strong(
            old_rip, rip, std::memory_order_relaxed)) {
        st.chain_top_count[victim].store(1, std::memory_order_relaxed);
    }
}

}  // namespace

bool perf_window_active() noexcept {
    // Out-of-line form kept for callers outside hot paths; hot callers should
    // use the inlinable `perf_window_active_fast()` in perf_window.hpp.
    return perf_window_active_fast();
}

void perf_add_jit_insns(u64 n) noexcept {
    if (!perf_window_active() || n == 0) return;
    state().jit_insns.fetch_add(n, std::memory_order_relaxed);
    thread_acc().jit.fetch_add(n, std::memory_order_relaxed);
}

void perf_add_interp_insns(u64 n) noexcept {
    if (!perf_window_active() || n == 0) return;
    state().interp_insns.fetch_add(n, std::memory_order_relaxed);
    thread_acc().interp.fetch_add(n, std::memory_order_relaxed);
}

void perf_add_fallback_insns(u64 n) noexcept {
    if (!perf_window_active() || n == 0) return;
    state().fallback_insns.fetch_add(n, std::memory_order_relaxed);
    thread_acc().fallback.fetch_add(n, std::memory_order_relaxed);
}

void perf_add_dispatcher_entry(u32 hops) noexcept {
    if (!perf_window_active()) return;
    state().dispatcher_entries.fetch_add(1, std::memory_order_relaxed);
    if (hops != 0) {
        state().linked_hops.fetch_add(hops, std::memory_order_relaxed);
    }
}

void perf_note_hop_limit_fallback() noexcept {
    if (!perf_window_active()) return;
    state().hop_limit_fallbacks.fetch_add(1, std::memory_order_relaxed);
}

// ---- BEGIN dispatcher exit-reason diagnostic (self-contained block) --------
void perf_add_jit_exit_unattributed_terminal(u32 opcode) noexcept {
    if (!perf_window_active()) return;
    if (opcode >= State::kTerminalOpcodes) opcode = State::kTerminalOpcodes - 1;
    state().jit_exit_unattr_terminal[opcode].fetch_add(1, std::memory_order_relaxed);
}

void perf_add_jit_exit(u32 reason, u32 result_code,
                       bool pending_raise) noexcept {
    if (!perf_window_active()) return;
    auto& st = state();
    // Fold an out-of-range reason into Unattributed instead of dropping it:
    // the histogram must always sum to dispatcher_entries.
    const u32 bucket = (reason < kPerfJitExitReasonCount) ? reason : 0u;
    st.jit_exit_reason[bucket].fetch_add(1, std::memory_order_relaxed);
    if (bucket == 0) {
        const u32 r = (result_code < kPerfJitExecResultCount)
                          ? result_code
                          : (kPerfJitExecResultCount - 1);
        st.jit_exit_unattr_result[r].fetch_add(1, std::memory_order_relaxed);
        if (pending_raise) {
            st.jit_exit_unattr_raise.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void perf_add_link_predictor(u32 sticky0, u32 sticky1, u32 rsb, u32 ret_l1,
                             u32 ind_l1) noexcept {
    if (!perf_window_active()) return;
    auto& st = state();
    if (sticky0) st.ret_sticky0_hits.fetch_add(sticky0, std::memory_order_relaxed);
    if (sticky1) st.ret_sticky1_hits.fetch_add(sticky1, std::memory_order_relaxed);
    if (rsb) st.ret_rsb_hits.fetch_add(rsb, std::memory_order_relaxed);
    if (ret_l1) st.ret_l1_hits.fetch_add(ret_l1, std::memory_order_relaxed);
    if (ind_l1) st.ind_l1_hits.fetch_add(ind_l1, std::memory_order_relaxed);
}
// ---- END dispatcher exit-reason diagnostic ---------------------------------

void perf_add_helper_mem(bool is_write, bool fault) noexcept {
    // Same counters as `perf_add_helper_mem_fast()`; kept as an out-of-line
    // entry point for callers that are not on a hot path.
    perf_add_helper_mem_fast(is_write, fault);
}

void perf_note_helper_write_page(u64 addr) noexcept {
    if (!perf_window_active()) return;
    note_helper_write_page(addr);
}

void perf_note_helper_write_reason(u8 check_bits, u8 next_bits) noexcept {
    if (!perf_window_active()) return;
    note_helper_write_reason(check_bits, next_bits);
}

void perf_note_helper_write_symbol(u64 host_pc) noexcept {
    if (!perf_window_active()) return;
    note_helper_write_symbol(host_pc);
}

void perf_note_helper_read(u64 addr, int width) noexcept {
    if (!perf_window_active()) return;
    note_helper_read(addr, width);
}

void perf_note_helper_read_cross(bool cross) noexcept {
    if (!perf_window_active()) return;
    note_helper_read_cross(cross);
}

void perf_note_helper_read_reason(u8 check_bits) noexcept {
    if (!perf_window_active()) return;
    note_helper_read_reason(check_bits);
}

void perf_note_helper_read_site(u64 guest_rip) noexcept {
    if (!perf_window_active()) return;
    note_helper_read_site(guest_rip);
}

void perf_note_helper_write_width(int width) noexcept {
    if (!perf_window_active()) return;
    note_helper_write_width(width);
}

bool perf_helper_write_site_sample_due() noexcept {
    if (!perf_window_active()) return false;
    return write_site_sample_due();
}

void perf_note_helper_write_site(u64 guest_rip, u64 host_pc,
                                 int kind) noexcept {
    if (!perf_window_active()) return;
    note_helper_write_site(guest_rip, host_pc, kind);
}

void perf_add_linear_mem_emit(bool linear) noexcept {
    if (!perf_window_active()) return;
    if (linear) {
        state().linear_mem_emits.fetch_add(1, std::memory_order_relaxed);
    } else {
        state().helper_mem_emits.fetch_add(1, std::memory_order_relaxed);
    }
}

void perf_add_rep_movs(u64 bytes) noexcept {
    if (!perf_window_active()) return;
    state().rep_movs_calls.fetch_add(1, std::memory_order_relaxed);
    state().rep_movs_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void perf_add_rep_stos(u64 bytes) noexcept {
    if (!perf_window_active()) return;
    state().rep_stos_calls.fetch_add(1, std::memory_order_relaxed);
    state().rep_stos_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void perf_add_fused_memcpy(u64 bytes) noexcept {
    if (!perf_window_active()) return;
    state().fused_memcpy_calls.fetch_add(1, std::memory_order_relaxed);
    state().fused_memcpy_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void perf_add_fused_memset(u64 bytes) noexcept {
    if (!perf_window_active()) return;
    state().fused_memset_calls.fetch_add(1, std::memory_order_relaxed);
    state().fused_memset_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void perf_add_wait(PerfWaitKind kind, u64 ns) noexcept {
    if (!perf_window_active()) return;
    const u32 i = static_cast<u32>(kind);
    if (i >= kWaitKindCount) return;
    auto& w = state().wait[i];
    w.calls.fetch_add(1, std::memory_order_relaxed);
    w.ns.fetch_add(ns, std::memory_order_relaxed);
    fetch_max(w.max_ns, ns);
}

void perf_add_thunk(PerfThunkKind kind, u64 ns) noexcept {
    if (!perf_window_active()) return;
    const u32 i = static_cast<u32>(kind);
    if (i >= kThunkKindCount) return;
    auto& t = state().thunk[i];
    t.calls.fetch_add(1, std::memory_order_relaxed);
    t.ns.fetch_add(ns, std::memory_order_relaxed);
    fetch_max(t.max_ns, ns);
}

void perf_add_fs(PerfFsKind kind, u64 ns, u64 bytes) noexcept {
    if (!perf_window_active()) return;
    const u32 i = static_cast<u32>(kind);
    if (i >= kFsKindCount) return;
    auto& f = state().fs[i];
    f.calls.fetch_add(1, std::memory_order_relaxed);
    f.ns.fetch_add(ns, std::memory_order_relaxed);
    f.bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void perf_add_registry(u64 ns) noexcept {
    if (!perf_window_active()) return;
    state().registry_calls.fetch_add(1, std::memory_order_relaxed);
    state().registry_ns.fetch_add(ns, std::memory_order_relaxed);
}

void perf_add_present() noexcept {
    if (!perf_window_active()) return;
    state().presents.fetch_add(1, std::memory_order_relaxed);
}

void perf_add_draw() noexcept {
    if (!perf_window_active()) return;
    state().draws.fetch_add(1, std::memory_order_relaxed);
}

void perf_add_graphics_ns(u64 ns) noexcept {
    if (!perf_window_active() || ns == 0) return;
    state().graphics_ns.fetch_add(ns, std::memory_order_relaxed);
}

void perf_note_threads(u32 guest_active, u32 guest_runnable,
                       u32 guest_suspended, u32 host_workers) noexcept {
    if (!perf_window_active()) return;
    auto& st = state();
    st.guest_threads.store(guest_active, std::memory_order_relaxed);
    st.guest_runnable.store(guest_runnable, std::memory_order_relaxed);
    st.guest_suspended.store(guest_suspended, std::memory_order_relaxed);
    st.host_workers.store(host_workers, std::memory_order_relaxed);
    u32 cur = st.guest_threads_max.load(std::memory_order_relaxed);
    while (guest_active > cur &&
           !st.guest_threads_max.compare_exchange_weak(
               cur, guest_active, std::memory_order_relaxed)) {
    }
    cur = st.host_workers_max.load(std::memory_order_relaxed);
    while (host_workers > cur &&
           !st.host_workers_max.compare_exchange_weak(
               cur, host_workers, std::memory_order_relaxed)) {
    }
}

void perf_note_compile(bool success, bool memo_hit, u64 ns, u32 native_bytes,
                       u32 guest_insns, u32 guest_bytes,
                       u64 guest_addr) noexcept {
    if (!perf_window_active()) return;
    auto& st = state();
    if (memo_hit) {
        st.compile_memo_hits.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    st.compile_attempts.fetch_add(1, std::memory_order_relaxed);
    if (success) {
        st.compile_successes.fetch_add(1, std::memory_order_relaxed);
        st.compile_ns.fetch_add(ns, std::memory_order_relaxed);
        fetch_max(st.compile_ns_max, ns);
        st.native_bytes.fetch_add(native_bytes, std::memory_order_relaxed);
        const u32 b = size_bucket(guest_insns);
        st.compile_size_count[b].fetch_add(1, std::memory_order_relaxed);
        st.compile_size_ns[b].fetch_add(ns, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(st.compile_mu);
        if (st.compiles.size() < kMaxCompileRecs) {
            st.compiles.push_back(CompileRec{guest_addr, guest_insns,
                                             guest_bytes, native_bytes, ns});
        }
        // ---- BEGIN codegen size-model diagnostic accumulation --------------
        // Unbounded (unlike `compiles`), so the regression below is exact for
        // the whole window. Same lock, no extra synchronisation.
        {
            const u64 n = guest_insns;
            const u64 y = native_bytes;
            st.sm_blocks += 1;
            st.sm_sum_n += n;
            st.sm_sum_n2 += n * n;
            st.sm_sum_y += y;
            st.sm_sum_ny += n * y;
            st.sm_sum_gbytes += guest_bytes;
            if (y < st.sm_min_bytes) st.sm_min_bytes = y;
            const u32 h = n <= State::kSizeModelMaxN
                              ? static_cast<u32>(n)
                              : 0xFFFFFFFFu;
            if (h != 0xFFFFFFFFu) {
                st.sm_hist_count[h] += 1;
                st.sm_hist_bytes[h] += y;
            }
            st.sm_bucket_count[b] += 1;
            st.sm_bucket_bytes[b] += y;
            st.sm_bucket_insns[b] += n;
        }
        // ---- END codegen size-model diagnostic accumulation ----------------
    } else {
        st.compile_failures.fetch_add(1, std::memory_order_relaxed);
    }
}

void perf_note_memory_model(bool guest_linear, bool jit_linear,
                            bool jit_linear64, bool no_jitcache) noexcept {
    auto& st = state();
    st.guest_linear.store(guest_linear, std::memory_order_relaxed);
    st.jit_linear.store(jit_linear, std::memory_order_relaxed);
    st.jit_linear64.store(jit_linear64, std::memory_order_relaxed);
    st.no_jitcache.store(no_jitcache, std::memory_order_relaxed);
}

void perf_note_block_miss(u64 rip, bool empty_decode) noexcept {
    note_miss_rip(rip, empty_decode, false);
}

void perf_note_chain_lookup_miss(u64 rip) noexcept {
    note_miss_rip(rip, false, true);
    if (perf_window_active()) {
        note_chain_top_miss(rip);
    }
}

void perf_note_chain_miss_mid_block() noexcept {
    if (!perf_window_active()) return;
    state().chain_miss_mid_block.fetch_add(1, std::memory_order_relaxed);
}

void perf_note_chain_miss_thunk() noexcept {
    if (!perf_window_active()) return;
    state().chain_miss_thunk.fetch_add(1, std::memory_order_relaxed);
}

void perf_note_chain_thunk_inline() noexcept {
    if (!perf_window_active()) return;
    state().chain_thunk_inline.fetch_add(1, std::memory_order_relaxed);
}

void perf_note_chain_thunk_inline_fast() noexcept {
    if (!perf_window_active()) return;
    auto& st = state();
    st.chain_thunk_inline.fetch_add(1, std::memory_order_relaxed);
    st.chain_thunk_inline_fast.fetch_add(1, std::memory_order_relaxed);
}

void perf_note_chain_thunk_inline_slow() noexcept {
    if (!perf_window_active()) return;
    auto& st = state();
    st.chain_thunk_inline.fetch_add(1, std::memory_order_relaxed);
    st.chain_thunk_inline_slow.fetch_add(1, std::memory_order_relaxed);
}

void perf_note_chain_thunk_inline_direct() noexcept {
    if (!perf_window_active()) return;
    auto& st = state();
    st.chain_thunk_inline.fetch_add(1, std::memory_order_relaxed);
    st.chain_thunk_inline_direct.fetch_add(1, std::memory_order_relaxed);
}

void perf_note_jcc_emit_fused() noexcept {
    if (!perf_window_active()) return;
    state().jcc_emit_fused.fetch_add(1, std::memory_order_relaxed);
}

void perf_note_rep_emit_inline() noexcept {
    if (!perf_window_active()) return;
    state().rep_emit_inline.fetch_add(1, std::memory_order_relaxed);
}

void perf_note_cf_emit_known() noexcept {
    if (!perf_window_active()) return;
    state().cf_emit_known.fetch_add(1, std::memory_order_relaxed);
}

void perf_note_cf_emit_nzcv() noexcept {
    if (!perf_window_active()) return;
    state().cf_emit_nzcv.fetch_add(1, std::memory_order_relaxed);
}

void perf_note_cf_emit_dispatch() noexcept {
    if (!perf_window_active()) return;
    state().cf_emit_dispatch.fetch_add(1, std::memory_order_relaxed);
}

void perf_note_rep_emit_helper() noexcept {
    if (!perf_window_active()) return;
    state().rep_emit_helper.fetch_add(1, std::memory_order_relaxed);
}

void perf_note_jcc_emit_carried() noexcept {
    if (!perf_window_active()) return;
    state().jcc_emit_carried.fetch_add(1, std::memory_order_relaxed);
}

void perf_note_jcc_emit_helper_detail(u8 x86_cc, u8 nzcv_kind) noexcept {
    if (!perf_window_active()) return;
    state().jcc_helper_cc[x86_cc & 15u][nzcv_kind & 7u].fetch_add(
        1, std::memory_order_relaxed);
}

void perf_note_jcc_emit_helper() noexcept {
    if (!perf_window_active()) return;
    state().jcc_emit_helper.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace vitreo::diagnostics
