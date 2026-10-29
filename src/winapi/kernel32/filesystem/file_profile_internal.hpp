// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file file_profile_internal.hpp
/// @brief Optional FileManager I/O profiling (VITREO_PROFILE_FILES=1).

#include <vitreo/common.hpp>
#include <vitreo/diagnostics.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace vitreo::winapi {

struct FileOpStat {
    std::atomic<u64> count{0};
    std::atomic<u64> total_ns{0};
    std::atomic<u64> total_bytes{0};
};

struct FileProfileTable {
    FileOpStat create_file;
    FileOpStat write_file;
    FileOpStat read_file;
    bool enabled = false;
    std::chrono::steady_clock::time_point start;

    FileProfileTable() {
        const char* e = std::getenv("VITREO_PROFILE_FILES");
        enabled = (e && e[0] && e[0] != '0');
        start = std::chrono::steady_clock::now();
    }
    ~FileProfileTable() {
        if (!enabled) return;
        auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count();
        std::fprintf(stderr, "\n[file-profile] wall=%.3fs\n", wall_ns / 1e9);
        auto report = [](const char* name, FileOpStat& s) {
            u64 c = s.count.load();
            u64 ns = s.total_ns.load();
            u64 b = s.total_bytes.load();
            double t = ns / 1e9;
            double avg_us = c ? (double)ns / c / 1e3 : 0.0;
            double mb_s = (t > 0) ? (b / 1048576.0 / t) : 0.0;
            std::fprintf(stderr,
                "[file-profile] %-12s count=%-8llu total=%6.3fs avg=%7.1fus bytes=%-12llu thru=%.1f MiB/s\n",
                name,
                (unsigned long long)c, t, avg_us,
                (unsigned long long)b, mb_s);
        };
        report("CreateFile", create_file);
        report("WriteFile", write_file);
        report("ReadFile", read_file);
        std::fflush(stderr);
    }
};

extern FileProfileTable g_file_profile;

[[nodiscard]] inline bool file_profile_wanted() noexcept {
    return g_file_profile.enabled || diagnostics::perf_window_active();
}

inline void file_profile_record(FileOpStat& stat, diagnostics::PerfFsKind kind,
                                u64 ns, u64 bytes = 0) noexcept {
    if (g_file_profile.enabled) {
        stat.count.fetch_add(1, std::memory_order_relaxed);
        stat.total_ns.fetch_add(ns, std::memory_order_relaxed);
        if (bytes != 0) {
            stat.total_bytes.fetch_add(bytes, std::memory_order_relaxed);
        }
    }
    diagnostics::perf_add_fs(kind, ns, bytes);
}

} // namespace vitreo::winapi
