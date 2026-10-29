// SPDX-License-Identifier: LGPL-2.1-only

/// @file jit_helpers_mem_slow_stats.hpp
/// @brief VITREO_JIT_MEM_SLOW_STATS counters for the checked memory helpers.
///
/// PRIVATE: included only by jit_helpers_memory.cpp, whose helpers are the
/// only callers. Every entity in mem_slow_stats is already `inline` with
/// external linkage, so holding them in a header changes neither linkage
/// nor code generation.
///
/// Split out of jit_helpers.cpp.

#pragma once

#include "jit_helpers.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace vitreo::cpu::jit {

// VITREO_JIT_MEM_SLOW_STATS=1: diagnostics only. Why each access that reached
// a checked memory helper left the JIT's inline path, by the reason its host
// page carries in the linear check map (or that it straddles two host pages,
// or lies outside the reservation). This is what prices replacing a software
// check with hardware enforcement: an access class that is rare can afford a
// fault; a frequent one cannot. One relaxed increment per reason plus a
// stderr line every ~2 s, so a killed run still reports.
namespace mem_slow_stats {
enum Cls : u32 { kOutside, kCross, kMixedRead, kMixedProt, kCode, kWatched,
                 kClean, kCount };
inline const bool g_enabled =
    std::getenv("VITREO_JIT_MEM_SLOW_STATS") != nullptr;
inline std::atomic<u64> g_n[2][kCount]{};
inline std::atomic<u64> g_calls{0};
inline std::atomic<u64> g_last_ns{0};
// Mixed-page accesses by [write][log2 width].
inline std::atomic<u64> g_mixed_width[2][4]{};
// Sampled hot host pages of mixed-page accesses: one in 64, open addressing,
// key 0 = empty. Racy by design (diagnostic counts).
constexpr usize kPageSlots = 4096;
inline std::atomic<u64> g_page_key[kPageSlots]{};
inline std::atomic<u64> g_page_hits[kPageSlots]{};
inline std::atomic<u64> g_page_write_hits[kPageSlots]{};
inline void note_page(usize page, bool write) noexcept {
    const u64 key = static_cast<u64>(page) + 1;
    usize h = static_cast<usize>((key * 0x9E3779B97F4A7C15ull) >> 52) & (kPageSlots - 1);
    for (usize probe = 0; probe < 16; ++probe, h = (h + 1) & (kPageSlots - 1)) {
        u64 cur = g_page_key[h].load(std::memory_order_relaxed);
        if (cur == 0 && g_page_key[h].compare_exchange_strong(cur, key)) cur = key;
        if (cur == key) {
            (write ? g_page_write_hits : g_page_hits)[h].fetch_add(
                1, std::memory_order_relaxed);
            return;
        }
    }
}

// Outside-the-reservation accesses by owning mapping (one map_region: the
// VirtualAlloc reservation, view or image around the address), one in 64
// sampled. The mapping base names the reservation; the `[vmem] above-window`
// line VirtualAlloc logs for it carries the reserving call's type, protection
// and whether lpAddress was supplied. Unmapped addresses key on their 1 GiB
// slot so wild pointers still show where they point. Same racy table shape
// as the mixed-page sampler.
constexpr usize kOutsideSlots = 256;
inline std::atomic<u64> g_outside_key[kOutsideSlots]{};
inline std::atomic<u64> g_outside_size[kOutsideSlots]{};
inline std::atomic<u64> g_outside_hits[2][kOutsideSlots]{};
inline std::atomic<u64> g_outside_seen{0};
inline void note_outside(JitContext* ctx, GuestAddr addr, bool write) noexcept {
    if ((g_outside_seen.fetch_add(1, std::memory_order_relaxed) & 63u) != 0) return;
    GuestAddr map_base = 0;
    usize map_size = 0;
    if (ctx == nullptr || ctx->memory == nullptr ||
        !ctx->memory->query_mapping(addr, map_base, map_size)) {
        map_base = addr & ~((GuestAddr{1} << 30) - 1);
        map_size = 0;  // unmapped
    }
    const u64 key = static_cast<u64>(map_base) + 1;
    usize h = static_cast<usize>((key * 0x9E3779B97F4A7C15ull) >> 56) & (kOutsideSlots - 1);
    for (usize probe = 0; probe < 16; ++probe, h = (h + 1) & (kOutsideSlots - 1)) {
        u64 cur = g_outside_key[h].load(std::memory_order_relaxed);
        if (cur == 0 && g_outside_key[h].compare_exchange_strong(cur, key)) {
            cur = key;
            g_outside_size[h].store(map_size, std::memory_order_relaxed);
        }
        if (cur == key) {
            g_outside_hits[write ? 1 : 0][h].fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

inline void note(JitContext* ctx, GuestAddr addr, int width, bool write) noexcept {
    std::atomic<u64>* n = g_n[write ? 1 : 0];
    const auto bump = [&](Cls c) { n[c].fetch_add(1, std::memory_order_relaxed); };
    const u8* cm = (ctx != nullptr && ctx->memory != nullptr)
                       ? ctx->memory->linear_check_page_map()
                       : nullptr;
    const usize i = static_cast<usize>(addr) >> vitreo::kLinearHostPageShift;
    if (cm == nullptr || i >= ctx->memory->linear_check_page_count()) {
        bump(kOutside);
        note_outside(ctx, addr, write);
    } else {
        const GuestAddr last = addr + static_cast<GuestAddr>(width) - 1;
        if (((addr ^ last) >> vitreo::kLinearHostPageShift) != 0) bump(kCross);
        const u8 b = __atomic_load_n(&cm[i], __ATOMIC_RELAXED);
        if (b & kLinearCheckMixedRead) {
            bump(kMixedRead);
            const int lw = width >= 8 ? 3 : width >= 4 ? 2 : width >= 2 ? 1 : 0;
            g_mixed_width[write ? 1 : 0][lw].fetch_add(1, std::memory_order_relaxed);
        }
        if (b != 0 && (n[kMixedRead].load(std::memory_order_relaxed) & 63u) == 0) {
            note_page(i, write);
        }
        if (b & kLinearCheckMixedProt) bump(kMixedProt);
        if (b & kLinearCheckHoldsCode) bump(kCode);
        if (b & kLinearCheckWatched) bump(kWatched);
        if (b == 0) bump(kClean);
    }
    if ((g_calls.fetch_add(1, std::memory_order_relaxed) & 0xFFFu) != 0) return;
    const u64 now = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    u64 last_ns = g_last_ns.load(std::memory_order_relaxed);
    if (now - last_ns < 2'000'000'000ull ||
        !g_last_ns.compare_exchange_strong(last_ns, now)) {
        return;
    }
    const auto get = [](int w, Cls c) {
        return static_cast<unsigned long long>(
            g_n[w][c].load(std::memory_order_relaxed));
    };
    {
        struct Hot { u64 page; u64 r; u64 w; };
        Hot top[40]{};
        for (usize k = 0; k < kPageSlots; ++k) {
            const u64 key = g_page_key[k].load(std::memory_order_relaxed);
            if (key == 0) continue;
            const Hot h{key - 1, g_page_hits[k].load(std::memory_order_relaxed),
                        g_page_write_hits[k].load(std::memory_order_relaxed)};
            for (usize t = 0; t < 40; ++t) {
                if (h.r + h.w > top[t].r + top[t].w) {
                    for (usize m = 39; m > t; --m) top[m] = top[m - 1];
                    top[t] = h;
                    break;
                }
            }
        }
        for (const Hot& h : top) {
            if (h.r + h.w == 0) break;
            const GuestAddr base = static_cast<GuestAddr>(h.page)
                                   << vitreo::kLinearHostPageShift;
            // Each 4 KiB guest page's protection run, and the mapping (one
            // map_region: a stack, a heap reservation, an image) around it.
            char sub[160] = {0};
            usize used = 0;
            GuestAddr map_base = 0;
            usize map_size = 0;
            if (ctx != nullptr && ctx->memory != nullptr) {
                (void)ctx->memory->query_mapping(base, map_base, map_size);
                for (int k = 0; k < 4; ++k) {
                    GuestAddr rb = 0;
                    usize rs = 0;
                    MemoryProtection prot{};
                    const GuestAddr a = base + static_cast<GuestAddr>(k) * 4096;
                    const bool ok = ctx->memory->query_region(a, rb, rs, prot);
                    used += static_cast<usize>(std::snprintf(
                        sub + used, sizeof(sub) - used, " [%s%x run=%llx+%zx]",
                        ok ? "" : "unmapped ", static_cast<unsigned>(prot),
                        static_cast<unsigned long long>(rb), rs));
                    if (used >= sizeof(sub)) break;
                }
            }
            std::fprintf(stderr,
                         "[jit-mem-slow]   page 0x%llx reads=%llu writes=%llu byte=0x%x "
                         "mapping=%llx+%zx%s\n",
                         static_cast<unsigned long long>(base),
                         static_cast<unsigned long long>(h.r),
                         static_cast<unsigned long long>(h.w),
                         ctx != nullptr && ctx->memory != nullptr &&
                                 ctx->memory->linear_check_page_map() != nullptr
                             ? ctx->memory->linear_check_page_map()[h.page]
                             : 0u,
                         static_cast<unsigned long long>(map_base), map_size, sub);
        }
    }
    for (usize k = 0; k < kOutsideSlots; ++k) {
        const u64 key = g_outside_key[k].load(std::memory_order_relaxed);
        if (key == 0) continue;
        const usize size = static_cast<usize>(
            g_outside_size[k].load(std::memory_order_relaxed));
        std::fprintf(stderr,
                     "[jit-mem-slow]   outside %s 0x%llx+0x%zx reads~%llu "
                     "writes~%llu (x64 sampled)\n",
                     size != 0 ? "mapping" : "unmapped-1GiB",
                     static_cast<unsigned long long>(key - 1), size,
                     static_cast<unsigned long long>(
                         g_outside_hits[0][k].load(std::memory_order_relaxed) * 64),
                     static_cast<unsigned long long>(
                         g_outside_hits[1][k].load(std::memory_order_relaxed) * 64));
    }
    for (int w = 0; w < 2; ++w) {
        std::fprintf(stderr, "[jit-mem-slow] %s mixed by width: 1=%llu 2=%llu 4=%llu 8=%llu\n",
                     w ? "writes" : "reads ",
                     static_cast<unsigned long long>(g_mixed_width[w][0].load()),
                     static_cast<unsigned long long>(g_mixed_width[w][1].load()),
                     static_cast<unsigned long long>(g_mixed_width[w][2].load()),
                     static_cast<unsigned long long>(g_mixed_width[w][3].load()));
    }
    for (int w = 0; w < 2; ++w) {
        std::fprintf(stderr,
                     "[jit-mem-slow] %s outside=%llu cross=%llu mixed_read=%llu "
                     "mixed_prot=%llu code=%llu watched=%llu clean=%llu\n",
                     w ? "writes" : "reads ", get(w, kOutside), get(w, kCross),
                     get(w, kMixedRead), get(w, kMixedProt), get(w, kCode),
                     get(w, kWatched), get(w, kClean));
    }
}
}  // namespace mem_slow_stats

} // namespace vitreo::cpu::jit
