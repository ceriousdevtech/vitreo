// SPDX-License-Identifier: LGPL-2.1-only

/// @file jit_helpers_sse_histogram.hpp
/// @brief VITREO_JIT_SSE_HIST: the per-op_id call histogram of jit_sse_op.
///
/// PRIVATE: included ONLY by jit_helpers_sse.cpp. The definitions below have
/// internal linkage, so a second includer would silently get its own enable
/// flag and its own histogram. Keeping them in jit_sse_op's translation unit
/// also keeps the per-call record_sse_helper_call fast path inlinable.
///
/// Split out of jit_helpers.cpp.

#pragma once

#include "jit_helpers.hpp"

#include <vitreo/diagnostics.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <utility>
#include <vector>

namespace vitreo::cpu::jit {

constexpr usize kSseHelperHistogramSize = 1024;
constexpr u64 kSseHelperFirstReport = 256;
constexpr usize kSseHelperReportLimit = 32;

struct SseHelperHistogram {
    std::array<std::atomic<u64>, kSseHelperHistogramSize> counts{};
    std::atomic<u64> overflow{0};
    std::atomic<u64> total{0};
    std::atomic<u64> next_report{kSseHelperFirstReport};
    std::mutex report_mutex;
};

// Read once at static init rather than latched on first call, so the answer
// does not depend on when the first SSE helper happens to run.
static std::atomic<bool> g_sse_helper_histogram_enabled{
    std::getenv("VITREO_JIT_SSE_HIST") != nullptr};

static inline bool sse_helper_histogram_enabled() noexcept {
    return g_sse_helper_histogram_enabled.load(std::memory_order_relaxed);
}

static SseHelperHistogram& sse_helper_histogram() {
    static SseHelperHistogram histogram;
    return histogram;
}

static inline void record_sse_helper_call(u32 op_id) {
    diagnostics::phase_note_sse_helper();
    if (!sse_helper_histogram_enabled()) {
        return;
    }

    auto& histogram = sse_helper_histogram();
    if (op_id < histogram.counts.size()) {
        histogram.counts[op_id].fetch_add(1, std::memory_order_relaxed);
    } else {
        histogram.overflow.fetch_add(1, std::memory_order_relaxed);
    }

    const u64 total =
        histogram.total.fetch_add(1, std::memory_order_relaxed) + 1;
    u64 report_at = histogram.next_report.load(std::memory_order_relaxed);
    if (total < report_at
        || !histogram.next_report.compare_exchange_strong(
            report_at, report_at * 2,
            std::memory_order_relaxed, std::memory_order_relaxed)) {
        return;
    }

    std::lock_guard<std::mutex> lock(histogram.report_mutex);
    std::vector<std::pair<u32, u64>> top;
    top.reserve(histogram.counts.size());
    const u64 overflow =
        histogram.overflow.load(std::memory_order_relaxed);
    u64 snapshot_total = overflow;
    for (usize index = 0; index < histogram.counts.size(); ++index) {
        const u64 count =
            histogram.counts[index].load(std::memory_order_relaxed);
        if (count != 0) {
            top.emplace_back(static_cast<u32>(index), count);
            snapshot_total += count;
        }
    }
    std::sort(top.begin(), top.end(), [](const auto& left, const auto& right) {
        if (left.second != right.second) {
            return left.second > right.second;
        }
        return left.first < right.first;
    });

    std::fprintf(
        stderr,
        "[jit-sse-hist] total=%llu distinct=%zu overflow=%llu top=%zu\n",
        static_cast<unsigned long long>(snapshot_total),
        top.size(),
        static_cast<unsigned long long>(overflow),
        std::min(kSseHelperReportLimit, top.size()));
    for (usize index = 0;
         index < std::min(kSseHelperReportLimit, top.size());
         ++index) {
        const auto [reported_op, count] = top[index];
        const double share = snapshot_total == 0
            ? 0.0
            : 100.0 * static_cast<double>(count)
                / static_cast<double>(snapshot_total);
        std::fprintf(
            stderr,
            "[jit-sse-hist] op_id=%u count=%llu share=%.1f%%\n",
            reported_op,
            static_cast<unsigned long long>(count),
            share);
    }
    std::fflush(stderr);
}

} // namespace vitreo::cpu::jit
