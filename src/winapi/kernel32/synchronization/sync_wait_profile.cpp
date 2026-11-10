// SPDX-License-Identifier: LGPL-2.1-only

/// @file sync_wait_profile.cpp
/// @brief Sync wait profiling and causal-trace context helpers.

#include <pthread.h>
#include "sync_internal.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <tuple>
#include <utility>
#include <vector>

namespace vitreo::winapi {

namespace {

GuestAddr guest_return_address(Marshaler& m) {
    if (!m.has_cpu()) return 0;
    auto* cpu = m.cpu();
    if (cpu->is_64bit()) {
        return m.memory().read_u64(
            static_cast<GuestAddr>(cpu->gpr.rsp)).value_or(0);
    }
    return m.memory().read_u32(
        static_cast<GuestAddr>(cpu->sp())).value_or(0);
}

} // namespace

SyncTraceContext make_sync_trace_context(
    Marshaler& m, std::string_view api, bool capture_parents) {
    SyncTraceContext trace{api, guest_return_address(m)};
    if (!capture_parents || !m.has_cpu()) return trace;

    auto* cpu = m.cpu();
    if (cpu->is_64bit()) {
        // PE32+ needs unwind metadata rather than an RBP-only walk. Keep the
        // immediate return address until that generic unwinder is available.
        return trace;
    }

    const u64 stack_pointer = static_cast<u32>(cpu->sp());
    u64 frame_pointer = static_cast<u32>(cpu->gpr.rbp);
    const u64 stack_limit =
        std::min<u64>(0xffffffffULL, stack_pointer + 2ULL * 1024 * 1024);

    while (trace.parent_count < trace.parents.size()) {
        if ((frame_pointer & 3u) != 0 || frame_pointer < stack_pointer ||
            frame_pointer + 8 > stack_limit) {
            break;
        }

        const auto next = m.memory().read_u32(frame_pointer);
        const auto return_address = m.memory().read_u32(frame_pointer + 4);
        if (next.is_err() || return_address.is_err() ||
            return_address.value() == 0) {
            break;
        }

        trace.parents[trace.parent_count++] = return_address.value();
        if (next.value() <= frame_pointer || next.value() > stack_limit) break;
        frame_pointer = next.value();
    }
    return trace;
}

void log_long_wait(const char* api, u64 handle, u32 timeout_ms, u64 blocked_ns,
                   u64 caller) {
    static const u64 threshold_ns = [] {
        const char* e = std::getenv("VITREO_LONG_WAITS");
        if (e == nullptr) return ~u64{0};
        const long ms = std::strtol(e, nullptr, 10);
        return static_cast<u64>(ms > 1 ? ms : 20) * 1'000'000ULL;
    }();
    if (blocked_ns < threshold_ns) return;
    static const auto start = std::chrono::steady_clock::now();
    const double at = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    u64 tid = 0;
    pthread_threadid_np(nullptr, &tid);
    std::fprintf(stderr,
                 "[long-wait] t=%.3f tid=%llu %s handle=0x%llx timeout=%u "
                 "blocked=%.1fms caller=0x%llx\n",
                 at, static_cast<unsigned long long>(tid), api,
                 static_cast<unsigned long long>(handle), timeout_ms,
                 static_cast<double>(blocked_ns) / 1e6,
                 static_cast<unsigned long long>(caller));
}

// =============================================================================
// Wait profiling (gated by VITREO_PROFILE_WAITS=1)
// Records per-handle wait counts + total wait time, dumps on process exit.
// =============================================================================

WaitProfileTable::WaitProfileTable() {
    const char* e = std::getenv("VITREO_PROFILE_WAITS");
    enabled = (e && e[0] && e[0] != '0');
    live = std::getenv("VITREO_PROFILE_WAITS_LIVE") != nullptr;
    start = std::chrono::steady_clock::now();
}

WaitProfileTable::~WaitProfileTable() {
    if (enabled) dump();
}

WaitStat& WaitProfileTable::get(u32 h) {
    std::lock_guard<std::mutex> lk(mu);
    return per_handle[h];
}

void WaitProfileTable::maybe_dump() {
    if (!enabled || !live) return;
    const auto seconds = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count());
    u64 last = last_dump_second.load(std::memory_order_relaxed);
    if (seconds >= last + 10 &&
        last_dump_second.compare_exchange_strong(
            last, seconds, std::memory_order_relaxed)) {
        dump();
    }
}

void WaitProfileTable::dump() {
    if (!enabled) return;
    auto end = std::chrono::steady_clock::now();
    auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        end - start).count();
    std::fprintf(stderr,
        "\n[wait-profile] wall=%.3fs\n", wall_ns / 1e9);
    std::fprintf(stderr,
        "[wait-profile] Sleep: count=%llu total=%.3fs zero_ms=%llu\n",
        (unsigned long long)sleep_count.load(),
        sleep_total_ns.load() / 1e9,
        (unsigned long long)sleep_zero_count.load());
    // Sort by total wait time desc
    std::vector<std::pair<u32, std::tuple<u64, u64, u64, u64>>> rows;
    {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& kv : per_handle) {
            rows.emplace_back(kv.first,
                std::make_tuple(kv.second.total_ns.load(),
                                kv.second.count.load(),
                                kv.second.timeout_count.load(),
                                kv.second.zero_ms_count.load()));
        }
    }
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) {
                  return std::get<0>(a.second) > std::get<0>(b.second);
              });
    std::fprintf(stderr,
        "[wait-profile] WaitForSingleObject by handle (top 30 by total wait ns):\n");
    std::fprintf(stderr,
        "[wait-profile]   handle  count    total_s    avg_us  timeouts  poll(ms=0)\n");
    size_t n = std::min<size_t>(30, rows.size());
    for (size_t i = 0; i < n; ++i) {
        auto h = rows[i].first;
        auto [tot, cnt, to, zr] = rows[i].second;
        double tot_s = tot / 1e9;
        double avg_us = cnt ? (double)tot / cnt / 1e3 : 0.0;
        std::fprintf(stderr,
            "[wait-profile]   0x%04x  %6llu  %8.3f  %8.1f  %8llu  %8llu\n",
            h,
            (unsigned long long)cnt, tot_s, avg_us,
            (unsigned long long)to, (unsigned long long)zr);
    }
    // Aggregate totals
    u64 grand_count = 0, grand_ns = 0;
    for (auto& r : rows) {
        grand_count += std::get<1>(r.second);
        grand_ns += std::get<0>(r.second);
    }
    std::fprintf(stderr,
        "[wait-profile] TOTAL WaitForSingleObject: count=%llu total=%.3fs\n",
        (unsigned long long)grand_count, grand_ns / 1e9);
    std::fflush(stderr);
}

WaitProfileTable g_wait_profile;

} // namespace vitreo::winapi
