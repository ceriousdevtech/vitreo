// SPDX-License-Identifier: LGPL-2.1-only

/// @file perf_window_report_jit.cpp
/// @brief Perf report sections for the JIT: the codegen size model and the
///        dispatcher exit reasons with the return / indirect predictor.
///        Split out of perf_window.cpp as verbatim code motion.

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

namespace vitreo::diagnostics {
static const char* (*g_opcode_namer)(u32) = nullptr;

void perf_set_opcode_namer(const char* (*namer)(u32)) noexcept {
    g_opcode_namer = namer;
}

namespace perf_window_detail {

void report_codegen_size_model(ReportBuf& out, State& st) noexcept {
    // ---- BEGIN codegen size-model diagnostic report -------------------------
    // Self-contained block: reads only the sm_* fields accumulated in
    // perf_note_compile. Safe to move or drop wholesale.
    //
    // The question it answers: of the ~206 ARM instructions an average
    // compiled block emits, how many are per-BLOCK overhead (prologue,
    // pin load/store, RIP publication, lazy-flag persistence, epilogue,
    // shared cold exit stubs) and how many are the per-guest-instruction
    // lowerings themselves? Model:
    //
    //     emitted_bytes(block) = FIXED + PER_INSN * guest_insns
    //
    // reported in ARM instructions (bytes / 4, every emit() being one word).
    {
        u64 sm_blocks = 0, sum_n = 0, sum_n2 = 0, sum_y = 0, sum_ny = 0;
        u64 sum_gbytes = 0, min_bytes = 0;
        u64 h_count[State::kSizeModelMaxN + 1]{};
        u64 h_bytes[State::kSizeModelMaxN + 1]{};
        u64 b_count[kSizeBucketCount]{};
        u64 b_bytes[kSizeBucketCount]{};
        u64 b_insns[kSizeBucketCount]{};
        {
            std::lock_guard<std::mutex> lock(st.compile_mu);
            sm_blocks = st.sm_blocks;
            sum_n = st.sm_sum_n;
            sum_n2 = st.sm_sum_n2;
            sum_y = st.sm_sum_y;
            sum_ny = st.sm_sum_ny;
            sum_gbytes = st.sm_sum_gbytes;
            min_bytes = st.sm_min_bytes;
            for (u32 i = 0; i <= State::kSizeModelMaxN; ++i) {
                h_count[i] = st.sm_hist_count[i];
                h_bytes[i] = st.sm_hist_bytes[i];
            }
            for (u32 i = 0; i < kSizeBucketCount; ++i) {
                b_count[i] = st.sm_bucket_count[i];
                b_bytes[i] = st.sm_bucket_bytes[i];
                b_insns[i] = st.sm_bucket_insns[i];
            }
        }

        if (sm_blocks != 0) {
            const double dn = static_cast<double>(sm_blocks);
            out("\nCodegen size model (emitted ARM64 per compiled block):\n");
            out("  Blocks:                %llu\n",
                static_cast<unsigned long long>(sm_blocks));
            out("  Guest insns compiled:  %llu (%.2f/block)\n",
                static_cast<unsigned long long>(sum_n),
                static_cast<double>(sum_n) / dn);
            out("  Guest bytes compiled:  %llu\n",
                static_cast<unsigned long long>(sum_gbytes));
            out("  Native bytes emitted:  %llu (%llu ARM insns)\n",
                static_cast<unsigned long long>(sum_y),
                static_cast<unsigned long long>(sum_y / 4));
            out("  Mean:                  %.1f ARM/block, %.2f ARM/guest-insn"
                ", %.2f ARM/guest-byte\n",
                static_cast<double>(sum_y) / 4.0 / dn,
                sum_n ? static_cast<double>(sum_y) / 4.0 /
                            static_cast<double>(sum_n)
                      : 0.0,
                sum_gbytes ? static_cast<double>(sum_y) / 4.0 /
                                 static_cast<double>(sum_gbytes)
                           : 0.0);
            if (min_bytes != ~0ull) {
                out("  Smallest block:        %llu ARM insns\n",
                    static_cast<unsigned long long>(min_bytes / 4));
            }

            out("  By guest block size (count, ARM insns, means):\n");
            for (u32 i = 0; i < kSizeBucketCount; ++i) {
                const u64 c = b_count[i];
                if (c == 0) continue;
                out("    %-6s  %8llu blocks  %12llu ARM  "
                    "%7.1f ARM/block  %6.2f guest-insn/block  "
                    "%6.2f ARM/guest-insn\n",
                    size_bucket_name(i),
                    static_cast<unsigned long long>(c),
                    static_cast<unsigned long long>(b_bytes[i] / 4),
                    static_cast<double>(b_bytes[i]) / 4.0 /
                        static_cast<double>(c),
                    static_cast<double>(b_insns[i]) /
                        static_cast<double>(c),
                    b_insns[i] ? static_cast<double>(b_bytes[i]) / 4.0 /
                                     static_cast<double>(b_insns[i])
                               : 0.0);
            }

            // Exact per-guest-instruction-count histogram. This is the row
            // set the FIXED/PER_INSN split is really read off: the n=1 mean
            // is (FIXED + PER_INSN) directly, and the slope across adjacent
            // rows is PER_INSN with no bucket averaging in the way.
            out("  By exact guest instruction count:\n");
            for (u32 i = 1; i <= State::kSizeModelMaxN; ++i) {
                if (h_count[i] == 0) continue;
                out("    n=%-2u  %8llu blocks  %7.1f ARM/block  "
                    "%6.2f ARM/guest-insn\n",
                    i, static_cast<unsigned long long>(h_count[i]),
                    static_cast<double>(h_bytes[i]) / 4.0 /
                        static_cast<double>(h_count[i]),
                    static_cast<double>(h_bytes[i]) / 4.0 /
                        static_cast<double>(h_count[i] * i));
            }
            if (h_count[0] != 0) {
                out("    n=0   %8llu blocks (should not occur)\n",
                    static_cast<unsigned long long>(h_count[0]));
            }

            // Ordinary least squares over every compiled block, y in ARM
            // instructions. denom = N*S(n^2) - S(n)^2; it is zero only when
            // every block had the same guest instruction count, in which
            // case the split is not identifiable from this data at all.
            const double N = dn;
            const double Sn = static_cast<double>(sum_n);
            const double Sn2 = static_cast<double>(sum_n2);
            const double Sy = static_cast<double>(sum_y) / 4.0;
            const double Sny = static_cast<double>(sum_ny) / 4.0;
            const double denom = N * Sn2 - Sn * Sn;
            if (denom > 0.0) {
                const double per_insn = (N * Sny - Sn * Sy) / denom;
                const double fixed = (Sy - per_insn * Sn) / N;
                out("  Codegen: fixed ~%.1f ARM/block, ~%.1f ARM/guest-insn "
                    "(least squares, %llu samples)\n",
                    fixed, per_insn,
                    static_cast<unsigned long long>(sm_blocks));
                const double mean_n = Sn / N;
                const double mean_y = Sy / N;
                if (mean_y > 0.0) {
                    out("  Split of the %.1f ARM/block mean: %.1f fixed "
                        "(%.0f%%), %.1f lowering (%.0f%%)\n",
                        mean_y, fixed, 100.0 * fixed / mean_y,
                        per_insn * mean_n,
                        100.0 * per_insn * mean_n / mean_y);
                }
            } else {
                out("  Codegen: split not identifiable (all blocks had the "
                    "same guest instruction count)\n");
            }

            // Independent two-point check against the least-squares fit,
            // taken from the two exact-count rows that are furthest apart
            // and both populated. If the two disagree badly the model is not
            // linear and neither number should be trusted.
            u32 lo = 0, hi = 0;
            for (u32 i = 1; i <= State::kSizeModelMaxN; ++i) {
                if (h_count[i] == 0) continue;
                if (lo == 0) lo = i;
                hi = i;
            }
            if (lo != 0 && hi > lo) {
                const double y_lo = static_cast<double>(h_bytes[lo]) / 4.0 /
                                    static_cast<double>(h_count[lo]);
                const double y_hi = static_cast<double>(h_bytes[hi]) / 4.0 /
                                    static_cast<double>(h_count[hi]);
                const double per = (y_hi - y_lo) /
                                   static_cast<double>(hi - lo);
                out("  Two-point check (n=%u vs n=%u): fixed ~%.1f "
                    "ARM/block, ~%.1f ARM/guest-insn\n",
                    lo, hi, y_lo - per * static_cast<double>(lo), per);
            }
        }
    }
    // ---- END codegen size-model diagnostic report ---------------------------
}

void report_jit_exit_reasons(ReportBuf& out, State& st, u64 disp) noexcept {
    // ---- BEGIN dispatcher exit-reason diagnostic report ---------------------
    // Self-contained block: reads only the jit_exit_* / *_hits counters and
    // the already-computed `disp`. Safe to move or drop wholesale.
    {
        // Order MUST match cpu::jit::JitExitReason (jit_block.hpp). Kept as
        // a local table so core/base takes no dependency on core/cpu; the
        // bucket count is asserted against the enum in jit_compiler.cpp.
        static const char* const kReasonName[] = {
            "Unattributed (no reason stored)",
            "const tail: thunk target",
            "const tail: linking disabled",
            "const tail: L1 slot empty",
            "const tail: L1 addr mismatch",
            "const tail: target not compiled",
            "const tail: hop cap",
            "RET tail: linking disabled",
            "RET tail: L1 slot empty",
            "RET tail: L1 addr mismatch",
            "RET tail: target not compiled",
            "RET tail: hop cap",
            "indirect tail: linking disabled",
            "indirect tail: L1 slot empty",
            "indirect tail: L1 addr mismatch",
            "indirect tail: target not compiled",
            "indirect tail: hop cap",
            "epilogue: raised exception",
            "epilogue: zero-fill loop re-dispatch",
            "epilogue: truncated at uncompilable insn",
            "epilogue: block end without branch",
            "indirect tail: target is an API thunk",
            "const tail: target is an API thunk (runtime)",
        };
        constexpr u32 kNamed =
            static_cast<u32>(sizeof(kReasonName) / sizeof(kReasonName[0]));
        static const char* const kResultName[] = {
            "Continue", "ThunkCall", "Halt", "Unsupported", "StaleCode",
            "result=5", "result=6", "result>=7",
        };

        u64 attributed = 0;
        for (u32 i = 0; i < kPerfJitExitReasonCount; ++i) {
            attributed += st.jit_exit_reason[i].load(std::memory_order_relaxed);
        }
        const double denom =
            (disp != 0) ? static_cast<double>(disp) : 1.0;
        out("\nDispatcher exit reasons (why translated code returned to C++):\n");
        if (attributed == 0) {
            out("  (no samples; set VITREO_JIT_EXIT_STATS=1 to instrument"
                " generated code)\n");
        }
        for (u32 i = 0; i < kPerfJitExitReasonCount; ++i) {
            const u64 n = st.jit_exit_reason[i].load(std::memory_order_relaxed);
            if (n == 0) continue;
            const char* name =
                (i < kNamed) ? kReasonName[i] : "reason (unknown code)";
            out("  %-34s %12llu  %5.1f%%\n", name,
                static_cast<unsigned long long>(n),
                100.0 * static_cast<double>(n) / denom);
        }
        const u64 unattr =
            st.jit_exit_reason[0].load(std::memory_order_relaxed);
        if (unattr != 0) {
            out("  Unattributed by JitExecResult:\n");
            for (u32 i = 0; i < kPerfJitExecResultCount; ++i) {
                const u64 n =
                    st.jit_exit_unattr_result[i].load(std::memory_order_relaxed);
                if (n == 0) continue;
                out("    %-32s %12llu  %5.1f%%\n", kResultName[i],
                    static_cast<unsigned long long>(n),
                    100.0 * static_cast<double>(n) / denom);
            }
            out("    %-32s %12llu\n", "with pending guest exception",
                static_cast<unsigned long long>(
                    st.jit_exit_unattr_raise.load(std::memory_order_relaxed)));
            {
                // Top block-terminating opcodes among unattributed exits.
                struct Row { u32 op; u64 n; };
                std::vector<Row> rows;
                u64 total = 0;
                for (u32 i = 0; i < State::kTerminalOpcodes; ++i) {
                    const u64 n = st.jit_exit_unattr_terminal[i].load(
                        std::memory_order_relaxed);
                    if (n) { rows.push_back({i, n}); total += n; }
                }
                std::sort(rows.begin(), rows.end(),
                          [](const Row& a, const Row& b) { return a.n > b.n; });
                if (!rows.empty()) {
                    out("    by terminating opcode (top 12):\n");
                    for (usize i = 0; i < rows.size() && i < 12; ++i) {
                        const char* name = g_opcode_namer
                            ? g_opcode_namer(rows[i].op) : nullptr;
                        out("      %-32s %10llu %5.1f%%\n",
                            name ? name : "?",
                            static_cast<unsigned long long>(rows[i].n),
                            total ? 100.0 * static_cast<double>(rows[i].n) /
                                        static_cast<double>(total) : 0.0);
                    }
                }
            }
        }
        if (attributed != disp) {
            out("  NOTE: attributed %llu != dispatcher entries %llu\n",
                static_cast<unsigned long long>(attributed),
                static_cast<unsigned long long>(disp));
        }

        const u64 s0 = st.ret_sticky0_hits.load(std::memory_order_relaxed);
        const u64 s1 = st.ret_sticky1_hits.load(std::memory_order_relaxed);
        const u64 rsb = st.ret_rsb_hits.load(std::memory_order_relaxed);
        const u64 rl1 = st.ret_l1_hits.load(std::memory_order_relaxed);
        const u64 il1 = st.ind_l1_hits.load(std::memory_order_relaxed);
        // A RET miss is a RET tail that reached the dispatcher: sticky ring,
        // RSB and L1 all failed to supply a host link.
        u64 ret_miss = 0;
        for (u32 i = 7; i <= 11 && i < kPerfJitExitReasonCount; ++i) {
            ret_miss += st.jit_exit_reason[i].load(std::memory_order_relaxed);
        }
        u64 ind_miss = 0;
        for (u32 i = 12; i <= 16 && i < kPerfJitExitReasonCount; ++i) {
            ind_miss += st.jit_exit_reason[i].load(std::memory_order_relaxed);
        }
        const u64 ret_total = s0 + s1 + rsb + rl1 + ret_miss;
        const u64 ind_total = il1 + ind_miss;
        if (ret_total != 0 || ind_total != 0) {
            const double rd =
                (ret_total != 0) ? static_cast<double>(ret_total) : 1.0;
            const double idn =
                (ind_total != 0) ? static_cast<double>(ind_total) : 1.0;
            out("\nReturn / indirect predictor (per tail execution):\n");
            out("  RET tails:             %llu\n",
                static_cast<unsigned long long>(ret_total));
            out("    sticky[0] hit:       %12llu  %5.1f%%\n",
                static_cast<unsigned long long>(s0),
                100.0 * static_cast<double>(s0) / rd);
            out("    sticky[1] hit:       %12llu  %5.1f%%\n",
                static_cast<unsigned long long>(s1),
                100.0 * static_cast<double>(s1) / rd);
            out("    RSB hit:             %12llu  %5.1f%%\n",
                static_cast<unsigned long long>(rsb),
                100.0 * static_cast<double>(rsb) / rd);
            out("    L1 hit (pred. miss): %12llu  %5.1f%%\n",
                static_cast<unsigned long long>(rl1),
                100.0 * static_cast<double>(rl1) / rd);
            out("    full miss -> C++:    %12llu  %5.1f%%\n",
                static_cast<unsigned long long>(ret_miss),
                100.0 * static_cast<double>(ret_miss) / rd);
            out("  Indirect JMP/CALL tails: %llu\n",
                static_cast<unsigned long long>(ind_total));
            out("    L1 hit:              %12llu  %5.1f%%\n",
                static_cast<unsigned long long>(il1),
                100.0 * static_cast<double>(il1) / idn);
            out("    full miss -> C++:    %12llu  %5.1f%%\n",
                static_cast<unsigned long long>(ind_miss),
                100.0 * static_cast<double>(ind_miss) / idn);
        }
    }
    // ---- END dispatcher exit-reason diagnostic report -----------------------
}

}  // namespace perf_window_detail

}  // namespace vitreo::diagnostics
