// SPDX-License-Identifier: LGPL-2.1-only

/// @file jit_inval_stats.hpp
/// @brief VITREO_JIT_INVAL_STATS=1: why translated blocks are invalidated and
///        whether their recompilation saw different guest bytes.
///
/// Diagnostics only. Every hook is a single relaxed load of `g_enabled` when
/// off. Prints a per-process line roughly every two seconds so a killed child
/// still reports.

#pragma once

#include "core/memory/exec_write_source.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unistd.h>
#include <unordered_map>

namespace vitreo::cpu::jit::inval_stats {

using Source = exec_write_source::Source;
constexpr u32 kSourceCount = exec_write_source::kCount;

inline const bool g_enabled = std::getenv("VITREO_JIT_INVAL_STATS") != nullptr;

inline std::atomic<u64> g_events[kSourceCount]{};
inline std::atomic<u64> g_removed[kSourceCount]{};
inline std::atomic<u64> g_first_compiles{0};
inline std::atomic<u64> g_recompile_same{0};
inline std::atomic<u64> g_recompile_changed{0};
inline std::atomic<u64> g_recompile_same_ns{0};
inline std::atomic<u64> g_recompile_changed_ns{0};
// Recompiled with no executable write in between: arena retirement, tier
// withdrawal, a failed re-decode, a per-thread cache eviction, or another
// thread translating (or adopting from the memo) the same block.
inline std::atomic<u64> g_recompile_unwritten{0};
inline std::atomic<u64> g_recompile_unwritten_ns{0};
inline std::atomic<u64> g_last_ns{0};

struct HashBook {
    std::mutex mutex;
    // addr -> {hash, invalidated since last compile}
    std::unordered_map<GuestAddr, std::pair<u64, bool>> map;
};
inline HashBook& book() {
    static HashBook* b = new HashBook();
    return *b;
}

inline const char* source_name(u32 s) {
    static const char* const names[kSourceCount] = {
        "store", "bulk", "protect", "flush", "decommit", "other"};
    return s < kSourceCount ? names[s] : "?";
}

inline u64 now_ns() noexcept {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline void maybe_print() noexcept {
    const u64 now = now_ns();
    u64 last = g_last_ns.load(std::memory_order_relaxed);
    if (now - last < 2'000'000'000ull ||
        !g_last_ns.compare_exchange_strong(last, now)) {
        return;
    }
    char buf[512];
    int used = std::snprintf(buf, sizeof(buf), "[jit-inval] pid=%d", getpid());
    for (u32 s = 0; s < kSourceCount && used > 0 &&
                    used < static_cast<int>(sizeof(buf));
         ++s) {
        used += std::snprintf(buf + used, sizeof(buf) - static_cast<usize>(used),
                              " %s=%llu/%llu", source_name(s),
                              static_cast<unsigned long long>(g_events[s].load()),
                              static_cast<unsigned long long>(g_removed[s].load()));
    }
    std::fprintf(stderr,
                 "%s (events/blocks) first=%llu recompile_same=%llu (%.3fs) "
                 "recompile_changed=%llu (%.3fs) recompile_unwritten=%llu (%.3fs)\n",
                 buf, static_cast<unsigned long long>(g_first_compiles.load()),
                 static_cast<unsigned long long>(g_recompile_same.load()),
                 static_cast<double>(g_recompile_same_ns.load()) / 1e9,
                 static_cast<unsigned long long>(g_recompile_changed.load()),
                 static_cast<double>(g_recompile_changed_ns.load()) / 1e9,
                 static_cast<unsigned long long>(g_recompile_unwritten.load()),
                 static_cast<double>(g_recompile_unwritten_ns.load()) / 1e9);
}

/// Called by the notifier around an observer call.
inline void note_event(usize size, usize removed) noexcept {
    if (!g_enabled) return;
    const Source s = exec_write_source::classify(size);
    g_events[s].fetch_add(1, std::memory_order_relaxed);
    g_removed[s].fetch_add(removed, std::memory_order_relaxed);
}

inline void note_removed(GuestAddr addr) noexcept {
    HashBook& b = book();
    std::lock_guard<std::mutex> lock(b.mutex);
    auto it = b.map.find(addr);
    if (it != b.map.end()) it->second.second = true;
}

/// Returns a classification token for note_compile_done.
/// 0 = first compile, 1 = same-bytes recompile, 2 = changed-bytes recompile,
/// 3 = recompile without an intervening invalidation (eviction/thread).
inline int note_compile(GuestAddr addr, u64 hash) noexcept {
    HashBook& b = book();
    std::lock_guard<std::mutex> lock(b.mutex);
    auto [it, inserted] = b.map.try_emplace(addr, hash, false);
    if (inserted) return 0;
    const bool was_invalidated = it->second.second;
    const bool same = it->second.first == hash;
    it->second = {hash, false};
    if (!was_invalidated) return 3;
    return same ? 1 : 2;
}

inline void note_compile_done(int cls, u64 ns) noexcept {
    switch (cls) {
        case 0: g_first_compiles.fetch_add(1, std::memory_order_relaxed); break;
        case 1:
            g_recompile_same.fetch_add(1, std::memory_order_relaxed);
            g_recompile_same_ns.fetch_add(ns, std::memory_order_relaxed);
            break;
        case 2:
            g_recompile_changed.fetch_add(1, std::memory_order_relaxed);
            g_recompile_changed_ns.fetch_add(ns, std::memory_order_relaxed);
            break;
        case 3:
            g_recompile_unwritten.fetch_add(1, std::memory_order_relaxed);
            g_recompile_unwritten_ns.fetch_add(ns, std::memory_order_relaxed);
            break;
        default: break;
    }
    maybe_print();
}

}  // namespace vitreo::cpu::jit::inval_stats
