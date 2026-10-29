// SPDX-License-Identifier: LGPL-2.1-only

/// @file perf_window_report_dispatch.cpp
/// @brief Perf report section for dispatch and the block cache: lookups,
///        miss RIPs, emit paths, top chain-miss RIPs, evictions and the live
///        cached-block gauge. Split out of perf_window.cpp as verbatim code
///        motion.

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
std::atomic<vitreo::diagnostics::PerfMissNameFn> g_miss_rip_name{nullptr};
}  // namespace

namespace vitreo::diagnostics {
namespace perf_window_detail {

void report_dispatch_cache(ReportBuf& out, State& st, u64 disp, u64 hops,
                           double link_ratio, u64 l1, double l1_rate,
                           u64 hash, u64 miss, u64 evict, u64 inval,
                           const CacheSum& cache) noexcept {
    out("\nDispatch / cache (C++ lookup; ARM L1 tails omitted):\n");
    out("  Dispatcher entries:    %llu\n",
                 static_cast<unsigned long long>(disp));
    out("  Linked hops:           %llu\n",
                 static_cast<unsigned long long>(hops));
    out("  Hop-limit fallbacks:   %llu\n",
                 static_cast<unsigned long long>(
                     st.hop_limit_fallbacks.load(std::memory_order_relaxed)));
    out("  Direct-link ratio:     %.1f%%\n", link_ratio);
    out("  L1 hits:               %llu (%.1f%% of C++ lookups)\n",
                 static_cast<unsigned long long>(l1), l1_rate);
    out("  Hash hits:             %llu\n",
                 static_cast<unsigned long long>(hash));
    out("  Full misses:           %llu\n",
                 static_cast<unsigned long long>(miss));
    out("  Miss RIPs (decode/chain, window):\n");
    out("    total/unique/repeat: %llu / %llu / %llu\n",
                 static_cast<unsigned long long>(
                     st.block_miss_total.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     st.block_miss_unique.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     st.block_miss_repeat.load(std::memory_order_relaxed)));
    out("    collisions:          %llu\n",
                 static_cast<unsigned long long>(
                     st.block_miss_collision.load(std::memory_order_relaxed)));
    out("    empty decode:        %llu (repeat %llu)\n",
                 static_cast<unsigned long long>(
                     st.block_miss_empty.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     st.block_miss_empty_repeat.load(std::memory_order_relaxed)));
    out("    chain lookup miss:   %llu\n",
                 static_cast<unsigned long long>(
                     st.block_miss_chain.load(std::memory_order_relaxed)));
    out("    chain mid-block:     %llu\n",
                 static_cast<unsigned long long>(
                     st.chain_miss_mid_block.load(std::memory_order_relaxed)));
    out("    chain thunk miss:    %llu\n",
                 static_cast<unsigned long long>(
                     st.chain_miss_thunk.load(std::memory_order_relaxed)));
    out("    chain thunk inline:  %llu (fast %llu / slow %llu / direct %llu)\n",
                 static_cast<unsigned long long>(
                     st.chain_thunk_inline.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     st.chain_thunk_inline_fast.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     st.chain_thunk_inline_slow.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     st.chain_thunk_inline_direct.load(std::memory_order_relaxed)));
    out(
                 "  Jcc emit paths:        fused %llu / carried %llu / helper %llu\n",
                 static_cast<unsigned long long>(
                     st.jcc_emit_fused.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     st.jcc_emit_carried.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     st.jcc_emit_helper.load(std::memory_order_relaxed)));
    {
        static const char* kCC[16] = {"O","NO","B","AE","E","NE","BE","A",
                                      "S","NS","P","NP","L","GE","LE","G"};
        static const char* kNZ[8] = {"None","Sub","And","Add","IncDec","Sub8",
                                     "Carried","NzOnly"};
        for (u32 c = 0; c < 16; ++c) {
            for (u32 k = 0; k < 8; ++k) {
                const u64 n = st.jcc_helper_cc[c][k].load(
                    std::memory_order_relaxed);
                if (n == 0) continue;
                out("    helper Jcc: J%-3s incoming=%-8s %llu\n", kCC[c],
                    kNZ[k], static_cast<unsigned long long>(n));
            }
        }
    }
    out(
                 "  REP emit paths:        inline %llu / helper %llu\n",
                 static_cast<unsigned long long>(
                     st.rep_emit_inline.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     st.rep_emit_helper.load(std::memory_order_relaxed)));
    out(
                 "  CF emit paths:         nzcv %llu / known %llu / dispatch %llu\n",
                 static_cast<unsigned long long>(
                     st.cf_emit_nzcv.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     st.cf_emit_known.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     st.cf_emit_dispatch.load(std::memory_order_relaxed)));
    {
        struct TopEntry {
            u64 rip;
            u64 count;
        };
        TopEntry top[State::kTopMissSlots]{};
        u32 n = 0;
        for (u32 i = 0; i < State::kTopMissSlots; ++i) {
            const u64 rip =
                st.chain_top_rip[i].load(std::memory_order_relaxed);
            const u64 count =
                st.chain_top_count[i].load(std::memory_order_relaxed);
            if (rip == 0 || count == 0) {
                continue;
            }
            top[n++] = {rip, count};
        }
        std::sort(top, top + n, [](const TopEntry& a, const TopEntry& b) {
            return a.count > b.count;
        });
        const u32 show = n < 16u ? n : 16u;
        if (show != 0) {
            out("    top chain-miss RIPs:\n");
            const auto name_fn =
                g_miss_rip_name.load(std::memory_order_relaxed);
            for (u32 i = 0; i < show; ++i) {
                char name[96]{};
                if (name_fn != nullptr) {
                    name_fn(top[i].rip, name, sizeof(name));
                    name[sizeof(name) - 1] = '\0';
                }
                if (name[0] != '\0') {
                    out(
                                 "      %2u. 0x%llx  count=%llu  (chain)  %s\n",
                                 i + 1u,
                                 static_cast<unsigned long long>(top[i].rip),
                                 static_cast<unsigned long long>(top[i].count),
                                 name);
                } else {
                    out(
                                 "      %2u. 0x%llx  count=%llu  (chain)\n",
                                 i + 1u,
                                 static_cast<unsigned long long>(top[i].rip),
                                 static_cast<unsigned long long>(top[i].count));
                }
            }
        }
    }
    out("  Evictions:             %llu\n",
                 static_cast<unsigned long long>(evict));
    out("  Invalidations:         %llu\n",
                 static_cast<unsigned long long>(inval));
    // Live gauge, not a window delta: blocks currently retained across every
    // per-thread cache in this process. Each one costs an LRU node, two hash
    // nodes, a page-index set entry, and (for delegate-bearing blocks) its
    // 144-bytes-per-instruction decoded array, so this is the number that
    // decides whether the per-thread capacity is the process's memory problem.
    out("  Cached blocks (live):  %llu\n",
                 static_cast<unsigned long long>(cache.size));
}

}  // namespace perf_window_detail

void perf_set_miss_rip_name_fn(PerfMissNameFn fn) noexcept {
    g_miss_rip_name.store(fn, std::memory_order_relaxed);
}

}  // namespace vitreo::diagnostics
