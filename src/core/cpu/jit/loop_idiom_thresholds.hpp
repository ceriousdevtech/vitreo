// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// WS2 loop-idiom crossover thresholds and per-call budget. Measured by
// tests/cpu/test_jit_loop_idiom_bench.cpp (VITREO_BENCH_WS2=1); the table
// and the rule are recorded beside the constants.

#pragma once

#include "core/cpu/jit/loop_idiom_desc.hpp"

namespace vitreo::cpu::jit::loop_idiom {

/// Functional floor: the bulk path needs at least one iteration to do.
inline constexpr u32 kMinItersFloor = 1;

/// Per-call budget (plan C15, responsiveness): one helper call moves at most
/// this many bytes, and never fewer iterations than the REP string helpers'
/// MAX_JIT_LOOP_ITERS (4095) chunk, before the block returns to the
/// dispatcher. 64 KiB is ~3-6 us of host memcpy/memchr, well inside the
/// 16 ms GUI pump the in-block loop cap protects.
inline constexpr u64 kMaxBulkBytesPerCall = 64 * 1024;
inline constexpr u64 kMinItersPerCall = 4095;

[[nodiscard]] constexpr u64 max_iters_per_call(u32 elem_bytes) noexcept {
    const u64 by_bytes = kMaxBulkBytesPerCall / (elem_bytes == 0 ? 1 : elem_bytes);
    return by_bytes > kMinItersPerCall ? by_bytes : kMinItersPerCall;
}

/// Lowering policy for one (kind, element width). `lowered` false keeps the
/// shape as classification only (translated body, no prologue). min_iters is
/// the crossover as a whole-loop trip count: the inline gate declines below
/// it (and the helper declines a data-exit loop that would bulk fewer than
/// min_iters - 2 iterations).
struct Lowering {
    bool lowered{false};
    u32 min_iters{kMinItersFloor};
};

// MEASURED TABLE: tests/cpu/test_jit_loop_idiom_bench.cpp LoopIdiomBench.Gate,
// 2026-10-29 22:25 rerun, Apple M-series 10P+4E, QOS_CLASS_USER_INTERACTIVE,
// two whole-suite runs of 11 interleaved On/Off batches (>= 2 ms each) per
// size, under the shared bench lock with 1-minute load 4.3-4.9 (no build or
// guest alongside). Cells are off/on on the best batch (min) per arm, mean of
// the two runs, with the prologue forced on and no threshold. "x10" is the
// program-rule crossover (smallest sampled size from which On is >= 10%
// faster there and at every larger size). "T" is the shipped threshold,
// min_iters * element bytes: the smallest sampled size from which the best
// batch is >= 1.2x at T and every larger size (keep rule), never below x10.
// "@T" is the shipping-configuration Gate ratio at T, run 1 / run 2. Below T
// the inline gate declines and runs the unchanged body.
//
//  pat kind  w   4     16    64    256   1K    4K    64K   x10(B) T(B)  @T         verdict
//  F1  fill  1   0.61  0.97  2.21  6.14  20.0  40.6  62.5     32    64  2.16/2.15  on, 64 it
//  F4  fill  1   0.53  0.69  1.52  4.21  12.0  25.2  38.2     64    64  1.51/1.45  on, 64 it
//  F3  fill  2   0.79  0.76  1.22  2.76  6.84  14.0  21.2     64    64  1.23/1.30  on, 32 it
//  F2  fill  4   0.89  0.42  0.52  0.99  2.00  4.40  7.66    512   512  1.49/1.40  on, 128 it
//  F5  fill  8   -     0.42  0.37  0.63  1.41  2.90  4.52   1024  1024  1.44/1.42  on, 128 it
//  F6  fill  16  -     0.88  0.76  2.14  5.93  13.8  19.1    128   128  1.22/1.21  on, 8 it
//  C1  copy  1   0.75  1.24  2.97  9.02  24.2  55.3  69.0     16    32  1.87/1.89  on, 32 it
//  C7  copy  1   0.58  0.85  2.24  6.13  17.9  37.4  43.4     32    32  1.45/1.36  on, 32 it
//  C3  copy  4   0.87  0.66  0.86  1.58  3.29  6.40  6.69    128   256  1.58/1.60  on, 64 it
//  C8  copy  8   -     0.61  0.60  1.01  1.85  3.49  3.36    512   512  1.34/1.55  on, 64 it
//  C9  copy  16  -     0.86  0.75  1.84  4.34  6.99  8.38    128   256  1.93/1.80  on, 16 it
//  S7  scan  1   0.69  1.30  3.13  6.42  9.74  10.7  11.2     16    16  1.32/1.23  on (memchr, gated)
//  S4  scan  1   0.84  1.61  3.39  5.66  8.10  8.89  9.54     16     -  -          OFF (no gate: strlen)
//  S5  scan  2   0.84  1.52  3.31  7.57  11.9  14.1  14.9     16     -  -          OFF (no gate: wcslen)
//  S6  scan  16  -     0.68  0.75  1.09  1.66  2.33  2.84    512     -  -          OFF (no gate)
//  M3  cmp   1   0.63  0.87  1.51  2.40  3.05  3.30  3.26     64    64  1.43/1.50  on, 64 it
//  M4  cmp   8   -     0.39  0.42  0.61  1.19  1.89  2.27   1024  2048  1.52/1.55  on, 256 it
//
// S4/S5/S6 have no inline trip gate (unbounded scan to a terminator), so
// every entry would call the helper; their 16 B cell is the forced cost on
// a 16 B string and a gate cannot decline short strings. They stay off by
// the no-gate rule (see lowered() below), not by the speedup bar.
// Widths with no gated loop measured (copy 2 B, scan 2/4/8/16 B, compare
// 2/4/16 B) are not lowered: nothing shows them reaching the gate.
// The per-width min_iters is the largest T among its patterns.
inline constexpr u32 kFill1Crossover = 64;
inline constexpr u32 kFill2Crossover = 32;
inline constexpr u32 kFill4Crossover = 128;
inline constexpr u32 kFill8Crossover = 128;     // 1024 B
inline constexpr u32 kFill16Crossover = 8;
inline constexpr u32 kCopy1Crossover = 32;
inline constexpr u32 kCopy4Crossover = 64;      // 256 B (x10 is 128 B, below 1.2x)
inline constexpr u32 kCopy8Crossover = 64;      // 512 B
inline constexpr u32 kCopy16Crossover = 16;
inline constexpr u32 kScan1Crossover = 16;
inline constexpr u32 kCompare1Crossover = 64;   // 64 B (32 B below 1.2x)
inline constexpr u32 kCompare8Crossover = 256;  // 2048 B (1024 B: 1.16x, 1.19x)

namespace detail {
inline constexpr Lowering kOff{false, kMinItersFloor};
inline constexpr Lowering kLowering[4][5] = {
    // elem:        1 B                          2 B                    4 B                    8 B                    16 B
    /* Fill    */ {{true, kFill1Crossover}, {true, kFill2Crossover}, {true, kFill4Crossover}, {true, kFill8Crossover}, {true, kFill16Crossover}},
    /* Copy    */ {{true, kCopy1Crossover}, kOff, {true, kCopy4Crossover}, {true, kCopy8Crossover}, {true, kCopy16Crossover}},
    /* Scan    */ {{true, kScan1Crossover}, kOff, kOff, kOff, kOff},
    /* Compare */ {{true, kCompare1Crossover}, kOff, kOff, {true, kCompare8Crossover}, kOff},
};
}  // namespace detail

[[nodiscard]] constexpr Lowering lowering(Kind kind, u8 elem_log2) noexcept {
    const u32 k = static_cast<u32>(kind);
    if (k >= 4 || elem_log2 > 4) return {};
    return detail::kLowering[k][elem_log2];
}

[[nodiscard]] constexpr u32 min_iters(Kind kind, u8 elem_log2) noexcept {
    const u32 m = lowering(kind, elem_log2).min_iters;
    return m < kMinItersFloor ? kMinItersFloor : m;
}

/// Whether a matched loop gets the prologue. A loop without an inline trip
/// gate (no induction exit: strlen/strcpy/compare-until-terminator) would
/// call the helper on every entry, which costs more than a short string's
/// whole loop; the 16 B no-regression rule cannot be met, so it is never
/// lowered. Otherwise the measured table decides.
[[nodiscard]] constexpr bool lowered(Kind kind, u8 elem_log2, bool has_inline_gate) noexcept {
    return has_inline_gate && lowering(kind, elem_log2).lowered;
}

}  // namespace vitreo::cpu::jit::loop_idiom
