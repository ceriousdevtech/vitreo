// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file perf_window_report_internal.hpp
/// @brief The perf report's single-write buffer (ReportBuf), size_bucket_name,
///        and the report sections dump_unlocked (perf_window_report.cpp)
///        calls in order: perf_window_report_jit.cpp (codegen size model,
///        dispatcher exit reasons), perf_window_report_dispatch.cpp
///        (dispatch / cache), perf_window_report_memory.cpp (memory, bulk
///        memory) and perf_window_report_runtime.cpp (synchronization,
///        thunks, filesystem, registry, graphics, threads). Each section
///        takes the dump_unlocked locals it reads, under the same names.
///        Include from those files only. Split out of perf_window.cpp as
///        verbatim code motion.

#include "perf_window_internal.hpp"

#include <cstdarg>
#include <cstdio>
#include <string>

namespace vitreo::diagnostics::perf_window_detail {

inline const char* size_bucket_name(u32 i) noexcept {
    switch (i) {
        case 0: return "1-4";
        case 1: return "5-16";
        case 2: return "17-64";
        default: return "65+";
    }
}

/// Accumulates one whole perf report and emits it with a SINGLE write.
///
/// Several processes share one stderr across every process, so
/// emitting a report through ~80 separate `fprintf` calls lets two processes'
/// reports interleave line by line. A reader that splits the log on the
/// report header then attributes one process's counters to another. The mixed
/// record is self-inconsistent: an observed example reported 629M linked hops
/// against 213M guest instructions, which no single process can produce.
/// Buffering the report and writing it once keeps each report contiguous, and
/// also cuts the syscall count of a dump that a terminating process may not
/// survive.
struct ReportBuf {
    std::string s;

    void append(const char* fmt, va_list ap) noexcept {
        va_list copy;
        va_copy(copy, ap);
        const int n = std::vsnprintf(nullptr, 0, fmt, copy);
        va_end(copy);
        if (n <= 0) return;
        const usize off = s.size();
        s.resize(off + static_cast<usize>(n) + 1);
        std::vsnprintf(s.data() + off, static_cast<usize>(n) + 1, fmt, ap);
        s.resize(off + static_cast<usize>(n));  // drop vsnprintf's NUL
    }

    void operator()(const char* fmt, ...) noexcept {
        va_list ap;
        va_start(ap, fmt);
        append(fmt, ap);
        va_end(ap);
    }

    void flush() noexcept {
        if (s.empty()) return;
        std::fwrite(s.data(), 1, s.size(), stderr);
        std::fflush(stderr);
        s.clear();
    }
};

void report_codegen_size_model(ReportBuf& out, State& st) noexcept;
void report_jit_exit_reasons(ReportBuf& out, State& st, u64 disp) noexcept;
void report_dispatch_cache(ReportBuf& out, State& st, u64 disp, u64 hops,
                           double link_ratio, u64 l1, double l1_rate,
                           u64 hash, u64 miss, u64 evict, u64 inval,
                           const CacheSum& cache) noexcept;
void report_memory(ReportBuf& out, State& st, u64 linear_emits,
                   u64 helper_emits, u64 mem_emits, u64 helper_r,
                   u64 helper_w) noexcept;
void report_runtime(ReportBuf& out, State& st, u64 window_ns, u64 wait_ns,
                    u64 wait_calls, u64 sleep_ns, u64 fs_ns, u64 fs_calls,
                    u64 fs_bytes) noexcept;

}  // namespace vitreo::diagnostics::perf_window_detail
