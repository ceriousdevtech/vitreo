// SPDX-License-Identifier: LGPL-2.1-only

/// @file perf_window_report_memory.cpp
/// @brief Perf report sections for guest memory: the linear model, helper
///        reads and writes with their page, reason, probe, width and site
///        histograms, and bulk memory. Split out of perf_window.cpp as
///        verbatim code motion.

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

namespace vitreo::diagnostics::perf_window_detail {

void report_memory(ReportBuf& out, State& st, u64 linear_emits,
                   u64 helper_emits, u64 mem_emits, u64 helper_r,
                   u64 helper_w) noexcept {
    out("\nMemory:\n");
    out(
                 "  Linear model:          guest=%s jit32=%s jit64=%s "
                 "no_jitcache=%s\n",
                 st.guest_linear.load(std::memory_order_relaxed) ? "yes" : "no",
                 st.jit_linear.load(std::memory_order_relaxed) ? "yes" : "no",
                 st.jit_linear64.load(std::memory_order_relaxed) ? "yes" : "no",
                 st.no_jitcache.load(std::memory_order_relaxed) ? "yes" : "no");
    out("  Linear emits (compile):%llu (%.1f%%)\n",
                 static_cast<unsigned long long>(linear_emits),
                 mem_emits ? 100.0 * static_cast<double>(linear_emits) /
                                 static_cast<double>(mem_emits)
                           : 0.0);
    out("  Helper emits (compile):%llu\n",
                 static_cast<unsigned long long>(helper_emits));
    out("  Helper reads:          %llu\n",
                 static_cast<unsigned long long>(helper_r));
    out("  Helper writes:         %llu\n",
                 static_cast<unsigned long long>(helper_w));
    out("  Guest memory faults:   %llu\n",
                 static_cast<unsigned long long>(
                     g_helper_mem.faults.load(std::memory_order_relaxed)));

    // Helper-write page histogram (see State::wpage_*). Printed only when the
    // helper actually ran, so an ordinary run gains no output.
    {
        auto& st = state();
        const u64 wtot = st.wpage_total.load(std::memory_order_relaxed);
        if (wtot != 0) {
            struct Slot { u64 page; u64 n; };
            Slot top[16]{};
            u32 distinct = 0;
            for (u32 i = 0; i < State::kWritePageSlots; ++i) {
                const u64 pg = st.wpage_addr[i].load(std::memory_order_relaxed);
                if (pg == 0) continue;
                ++distinct;
                const u64 n = st.wpage_count[i].load(std::memory_order_relaxed);
                u32 worst = 0;
                for (u32 k = 1; k < 16; ++k) {
                    if (top[k].n < top[worst].n) worst = k;
                }
                if (n > top[worst].n) { top[worst].page = pg; top[worst].n = n; }
            }
            for (u32 a = 0; a < 16; ++a) {
                for (u32 b = a + 1; b < 16; ++b) {
                    if (top[b].n > top[a].n) { Slot t = top[a]; top[a] = top[b]; top[b] = t; }
                }
            }
            out("  Helper-write pages:    %u distinct (sampled), "
                "%llu collisions\n",
                distinct,
                static_cast<unsigned long long>(
                    st.wpage_collisions.load(std::memory_order_relaxed)));
            u64 shown = 0;
            for (u32 i = 0; i < 16 && top[i].n; ++i) {
                shown += top[i].n;
                out("    0x%08llx  %llu  (%.1f%%)\n",
                    static_cast<unsigned long long>(top[i].page << 12),
                    static_cast<unsigned long long>(top[i].n),
                    100.0 * static_cast<double>(top[i].n) /
                        static_cast<double>(wtot));
            }
            static const char* kReason[8] = {
                "(none)", "mixed-prot", "holds-code", "mixed+code",
                "watched", "watched+mixed", "watched+code", "all three"};
            for (u32 i = 0; i < 8; ++i) {
                const u64 n = st.wreason[i].load(std::memory_order_relaxed);
                if (n == 0) continue;
                out("    reason %-14s %llu  (%.1f%%)\n", kReason[i],
                    static_cast<unsigned long long>(n),
                    100.0 * static_cast<double>(n) /
                        static_cast<double>(wtot));
            }
            {
                static const char* kP[4] = {
                    "neither page", "own page", "NEIGHBOUR page only",
                    "both pages"};
                for (u32 i = 0; i < 4; ++i) {
                    const u64 n = st.wprobe[i].load(std::memory_order_relaxed);
                    if (n == 0) continue;
                    out("    probe hit  %-20s %llu  (%.1f%%)\n", kP[i],
                        static_cast<unsigned long long>(n),
                        100.0 * static_cast<double>(n) /
                            static_cast<double>(wtot));
                }
            }
            // Width breakdown: says WHICH helper, which narrows the emitter.
            {
                static const char* kW[4] = {"write8", "write16", "write32",
                                            "write64"};
                for (u32 i = 0; i < 4; ++i) {
                    const u64 n = st.wwidth[i].load(std::memory_order_relaxed);
                    if (n == 0) continue;
                    out("    via %-10s %llu  (%.1f%%)\n", kW[i],
                        static_cast<unsigned long long>(n),
                        100.0 * static_cast<double>(n) /
                            static_cast<double>(wtot));
                }
            }
            // Guest instructions that issued them, sampled 1-in-N.
            {
                const u64 samp = st.wsite_sampled.load(std::memory_order_relaxed);
                if (samp != 0) {
                    struct S { u64 rip; u64 n; };
                    S top[12]{};
                    u32 distinct = 0;
                    for (u32 i = 0; i < State::kWriteSiteSlots; ++i) {
                        const u64 r = st.wsite_rip[i].load(std::memory_order_relaxed);
                        if (r == 0) continue;
                        ++distinct;
                        const u64 n = st.wsite_count[i].load(std::memory_order_relaxed);
                        u32 worst = 0;
                        for (u32 k = 1; k < 12; ++k) {
                            if (top[k].n < top[worst].n) worst = k;
                        }
                        if (n > top[worst].n) { top[worst].rip = r; top[worst].n = n; }
                    }
                    for (u32 a = 0; a < 12; ++a) {
                        for (u32 b = a + 1; b < 12; ++b) {
                            if (top[b].n > top[a].n) { S t = top[a]; top[a] = top[b]; top[b] = t; }
                        }
                    }
                    out("  Helper-write sites (1-in-%llu sample; %llu sampled, "
                        "%llu unmapped [%llu block cold-stub, %llu not in any "
                        "block], %u distinct guest RIPs):\n",
                        static_cast<unsigned long long>(State::kWriteSiteStride),
                        static_cast<unsigned long long>(samp),
                        static_cast<unsigned long long>(
                            st.wsite_unmapped.load(std::memory_order_relaxed)),
                        static_cast<unsigned long long>(
                            st.wsite_cold.load(std::memory_order_relaxed)),
                        static_cast<unsigned long long>(
                            st.wsite_foreign.load(std::memory_order_relaxed)),
                        distinct);
                    // Hottest raw host PCs among the unmapped samples.
                    {
                        struct H { u64 pc; u64 n; };
                        H th[8]{};
                        for (u32 i = 0; i < 512; ++i) {
                            const u64 pc = st.wsite_hostpc[i].load(std::memory_order_relaxed);
                            if (pc == 0) continue;
                            const u64 n = st.wsite_hostpc_n[i].load(std::memory_order_relaxed);
                            u32 worst = 0;
                            for (u32 k = 1; k < 8; ++k) if (th[k].n < th[worst].n) worst = k;
                            if (n > th[worst].n) { th[worst].pc = pc; th[worst].n = n; }
                        }
                        for (u32 a = 0; a < 8; ++a)
                            for (u32 b = a + 1; b < 8; ++b)
                                if (th[b].n > th[a].n) { H t = th[a]; th[a] = th[b]; th[b] = t; }
                        for (u32 i = 0; i < 8 && th[i].n; ++i) {
                            const char* sym = "";
                            for (u32 k = 0; k < 8; ++k) {
                                if (st.wsite_sym_pc[k].load(
                                        std::memory_order_relaxed) == th[i].pc) {
                                    sym = st.wsite_sym[k];
                                    break;
                                }
                            }
                            out("    host_pc=0x%llx  %llu   %s\n",
                                static_cast<unsigned long long>(th[i].pc),
                                static_cast<unsigned long long>(th[i].n), sym);
                        }
                    }
                    for (u32 i = 0; i < 12 && top[i].n; ++i) {
                        out("    rip=0x%08llx  %llu  (%.1f%% of samples)\n",
                            static_cast<unsigned long long>(top[i].rip),
                            static_cast<unsigned long long>(top[i].n),
                            100.0 * static_cast<double>(top[i].n) /
                                static_cast<double>(samp));
                    }
                }
            }
            {
                const u64 rt = st.rtotal.load(std::memory_order_relaxed);
                if (rt != 0) {
                    const u64 c1 = st.rcross[1].load(std::memory_order_relaxed);
                    const u64 c0 = st.rcross[0].load(std::memory_order_relaxed);
                    out("  Helper READS:          %llu\n",
                        static_cast<unsigned long long>(rt));
                    out("    straddles a 16 KiB host page: %llu  (%.1f%%)\n",
                        static_cast<unsigned long long>(c1),
                        100.0 * static_cast<double>(c1) /
                            static_cast<double>(rt));
                    out("    within one host page:         %llu  (%.1f%%)\n",
                        static_cast<unsigned long long>(c0),
                        100.0 * static_cast<double>(c0) /
                            static_cast<double>(rt));
                    static const char* kRR[8] = {
                        "(none)", "mixed-prot", "holds-code", "mixed+code",
                        "watched", "watched+mixed", "watched+code", "all"};
                    for (u32 i = 0; i < 8; ++i) {
                        const u64 n = st.rreason[i].load(std::memory_order_relaxed);
                        if (n == 0) continue;
                        out("    reason %-14s %llu  (%.1f%%)\n", kRR[i],
                            static_cast<unsigned long long>(n),
                            100.0 * static_cast<double>(n) /
                                static_cast<double>(rt));
                    }
                    static const char* kRW[4] = {"read8", "read16", "read32",
                                                 "read64"};
                    for (u32 i = 0; i < 4; ++i) {
                        const u64 n = st.rwidth[i].load(std::memory_order_relaxed);
                        if (n == 0) continue;
                        out("    via %-8s %llu  (%.1f%%)\n", kRW[i],
                            static_cast<unsigned long long>(n),
                            100.0 * static_cast<double>(n) /
                                static_cast<double>(rt));
                    }
                    struct R { u64 rip; u64 n; };
                    R rt8[8]{};
                    for (u32 i = 0; i < 512; ++i) {
                        const u64 r = st.rsite_rip[i].load(std::memory_order_relaxed);
                        if (r == 0) continue;
                        const u64 n = st.rsite_n[i].load(std::memory_order_relaxed);
                        u32 worst = 0;
                        for (u32 k = 1; k < 8; ++k) if (rt8[k].n < rt8[worst].n) worst = k;
                        if (n > rt8[worst].n) { rt8[worst].rip = r; rt8[worst].n = n; }
                    }
                    for (u32 a = 0; a < 8; ++a)
                        for (u32 b = a + 1; b < 8; ++b)
                            if (rt8[b].n > rt8[a].n) { R t = rt8[a]; rt8[a] = rt8[b]; rt8[b] = t; }
                    const u64 rs = st.rsite_sampled.load(std::memory_order_relaxed);
                    if (rs != 0) {
                        out("    read sites (1-in-%llu sample; %llu sampled, "
                            "%llu unmapped):\n",
                            static_cast<unsigned long long>(State::kWriteSiteStride),
                            static_cast<unsigned long long>(rs),
                            static_cast<unsigned long long>(
                                st.rsite_unmapped.load(std::memory_order_relaxed)));
                        for (u32 i = 0; i < 8 && rt8[i].n; ++i) {
                            out("      rip=0x%08llx  %llu\n",
                                static_cast<unsigned long long>(rt8[i].rip),
                                static_cast<unsigned long long>(rt8[i].n));
                        }
                    }
                }
            }
            out("    top-16 cover %.1f%% of %llu recorded helper writes\n",
                100.0 * static_cast<double>(shown) /
                    static_cast<double>(wtot),
                static_cast<unsigned long long>(wtot));
        }
    }

    out("\nBulk memory:\n");
    out("  REP MOVS*:             %llu calls  %llu bytes\n",
                 static_cast<unsigned long long>(
                     st.rep_movs_calls.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     st.rep_movs_bytes.load(std::memory_order_relaxed)));
    out("  REP STOS*:             %llu calls  %llu bytes\n",
                 static_cast<unsigned long long>(
                     st.rep_stos_calls.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     st.rep_stos_bytes.load(std::memory_order_relaxed)));
    out("  Fused memcpy:          %llu calls  %llu bytes\n",
                 static_cast<unsigned long long>(
                     st.fused_memcpy_calls.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     st.fused_memcpy_bytes.load(std::memory_order_relaxed)));
    out("  Fused memset/fill:     %llu calls  %llu bytes\n",
                 static_cast<unsigned long long>(
                     st.fused_memset_calls.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     st.fused_memset_bytes.load(std::memory_order_relaxed)));
}

}  // namespace vitreo::diagnostics::perf_window_detail
