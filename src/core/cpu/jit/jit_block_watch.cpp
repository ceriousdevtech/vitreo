// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_block_watch.cpp
/// @brief Block-entry diagnostics (VITREO_COUNT_RIP, VITREO_CODE_WATCH_*)
///
/// Split out of jit_compiler.cpp by pure code motion; see that file for
/// the JIT register convention.

#include "jit_crash_breadcrumbs.hpp"

#include "jit_compiler.hpp"
#include "jit_emit_internal.hpp"
#include "jit_symbol_map.hpp"
#include "jit_split_layout.hpp"
#include "ir/jit_ir.hpp"
#include "stencils.hpp"
#include "jit_coverage.hpp"
#include "jit_stats.hpp"
#include "core/cpu/decode/guest_mapped_reader.hpp"
#include "flag_liveness.hpp"
#include "jit_helpers.hpp"
#include "idiom_detector.hpp"
#include "linear_fault.hpp"
#include "hot_block_dump.hpp"
#include "../../memory/jit_region_registry.hpp"
#include "core/cpu/lock_prefix.hpp"
#include "../../memory/mem_watch.hpp"
#include "core/memory/write_watch_traps.hpp"
#include "core/math/host_fenv.hpp"
#include "core/base/perf_window.hpp"
#include "core/cpu/decode/opcodes.hpp"  // inlinable perf_window_active_fast()
#include <vitreo/diagnostics.hpp>

#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <sys/mman.h>
#include <pthread.h>
#include <unistd.h>
#include <libkern/OSCacheControl.h>
#include <malloc/malloc.h>
#include "jit_compile_internal.hpp"

namespace vitreo::cpu::jit {

using namespace jit_detail;

namespace {

// DIAGNOSTIC (VITREO_COUNT_RIP=<addr>[,<addr>...]): exact execution counts for
// specific guest block entry addresses.
//
// `block_profiles` in the run loop cannot answer this: direct-linked JIT hops
// never re-enter the dispatcher, so its counts are both incomplete and (for
// the hot-dump path) sampled 1:32. The increment emitted here sits AFTER
// m_link_entry_pos, which linked hops do execute, so the count is exact.
//
// Costs nothing when the variable is unset: the check happens at compile time
// and no code is emitted.
struct RipCounter {
    GuestAddr addr;
    u64 count;
};
std::array<RipCounter, 8> g_rip_counters{};
usize g_rip_counter_n = 0;

usize rip_counter_init() {
    const char* v = std::getenv("VITREO_COUNT_RIP");
    if (v == nullptr) return 0;
    usize n = 0;
    const char* p = v;
    while (*p != '\0' && n < g_rip_counters.size()) {
        char* end = nullptr;
        const unsigned long long a = std::strtoull(p, &end, 0);
        if (end == p) break;
        g_rip_counters[n].addr = static_cast<GuestAddr>(a);
        g_rip_counters[n].count = 0;
        ++n;
        p = (*end == ',') ? end + 1 : end;
    }
    std::atexit([] {
        for (usize i = 0; i < g_rip_counter_n; ++i) {
            std::fprintf(stderr, "[rip-count] 0x%08llx executed %llu times\n",
                         (unsigned long long)g_rip_counters[i].addr,
                         (unsigned long long)g_rip_counters[i].count);
        }
    });
    return n;
}
} // namespace

u64* rip_counter_slot(GuestAddr addr) {
    static const usize n = (g_rip_counter_n = rip_counter_init());
    for (usize i = 0; i < n; ++i) {
        if (g_rip_counters[i].addr == addr) return &g_rip_counters[i].count;
    }
    return nullptr;
}

// A diagnostic only: observe the same generic address set as the interpreter,
// including linked block entries. Never bake a guest module or layout here.
void report_watched_block(CpuState* cpu, GuestMemory* memory, u64 rip) {
    // Optional bounded history for heap-random addresses: record watched
    // entries quietly, then show records whose registers cover the trigger's
    // first pointer argument. All addresses and the match range are supplied
    // by the diagnostic caller, never by a guest identity.
    struct Record { u64 rip; u64 tid; std::array<u64, 16> registers; };
    static const usize capacity = [] {
        const char* value = std::getenv("VITREO_CODE_WATCH_RING");
        return value ? std::min<usize>(std::strtoull(value, nullptr, 0), 65536) : 0;
    }();
    static const u64 trigger = [] {
        const char* value = std::getenv("VITREO_CODE_WATCH_TRIGGER");
        return value ? std::strtoull(value, nullptr, 0) : 0;
    }();
    static const u64 range = [] {
        const char* value = std::getenv("VITREO_CODE_WATCH_RANGE");
        return value ? std::strtoull(value, nullptr, 0) : 0;
    }();
    static std::mutex history_mutex;
    static std::vector<Record> history(capacity);
    static u64 cursor = 0;
    static u32 dumps = 0;
    if (capacity != 0) {
        std::lock_guard<std::mutex> lock(history_mutex);
        Record record{rip, mem_watch_host_tid(), {}};
        for (u8 reg = 0; reg < 16; ++reg) record.registers[reg] = cpu->gpr.get64(reg);
        history[cursor++ % capacity] = record;
        if (rip != trigger || dumps++ >= 8) return;
        const u64 pointer = cpu->gpr.get64(1);
        std::fprintf(stderr, "[jit-code-watch-history pid=%d] pointer=0x%llx records=%llu\n",
                     static_cast<int>(getpid()),
                     (unsigned long long)pointer, (unsigned long long)cursor);
        const u64 first = cursor > capacity ? cursor - capacity : 0;
        for (u64 i = first; i < cursor; ++i) {
            const Record& previous = history[i % capacity];
            const bool matches = std::any_of(previous.registers.begin(), previous.registers.end(),
                [&](u64 address) { return address <= pointer && pointer - address <= range; });
            if (!matches) continue;
            std::fprintf(stderr, "[jit-code-watch-history pid=%d] event=%llu rip=0x%llx tid=%llu",
                static_cast<int>(getpid()),
                (unsigned long long)i, (unsigned long long)previous.rip,
                (unsigned long long)previous.tid);
            for (u8 reg = 0; reg < 16; ++reg)
                std::fprintf(stderr, " r%u=0x%llx", static_cast<unsigned>(reg),
                             (unsigned long long)previous.registers[reg]);
            std::fprintf(stderr, "\n");
        }
    }
    // VITREO_CODE_WATCH_MAX raises the report cap (default 64).
    static const u32 max_reports = [] {
        const char* value = std::getenv("VITREO_CODE_WATCH_MAX");
        return value ? static_cast<u32>(std::strtoul(value, nullptr, 0)) : 64u;
    }();
    static std::atomic<u32> hits{0};
    if (hits.fetch_add(1, std::memory_order_relaxed) >= max_reports) return;
    std::fprintf(stderr, "[jit-code-watch pid=%d] rip=0x%llx tid=%llu", static_cast<int>(getpid()),
                 (unsigned long long)rip, (unsigned long long)mem_watch_host_tid());
    for (u8 reg = 0; reg < 16; ++reg) {
        std::fprintf(stderr, " r%u=0x%llx", static_cast<unsigned>(reg),
                     (unsigned long long)cpu->gpr.get64(reg));
    }
    std::fprintf(stderr, "\n");
    // VITREO_CODE_WATCH_STRINGS: name the strings argument registers point at
    // (an API or module name passed to a resolver), ASCII or UTF-16.
    static const bool strings = std::getenv("VITREO_CODE_WATCH_STRINGS") != nullptr;
    if (strings && memory != nullptr) {
        for (u8 reg = 0; reg < 16; ++reg) {
            const u64 at = cpu->gpr.get64(reg);
            if (at < 0x10000) continue;
            std::string ascii;
            std::string wide;
            bool ascii_done = false;
            bool wide_done = false;
            for (u32 i = 0; i < 96 && !(ascii_done && wide_done); ++i) {
                if (!ascii_done) {
                    const auto c = memory->read_u8(at + i);
                    if (!c || c.value() == 0) ascii_done = true;
                    else if (c.value() < 0x20 || c.value() > 0x7e) { ascii.clear(); ascii_done = true; }
                    else ascii.push_back(static_cast<char>(c.value()));
                }
                if (!wide_done) {
                    const auto w = memory->read_u16(at + i * 2u);
                    if (!w || w.value() == 0) wide_done = true;
                    else if (w.value() < 0x20 || w.value() > 0x7e) { wide.clear(); wide_done = true; }
                    else wide.push_back(static_cast<char>(w.value()));
                }
            }
            const std::string& text = wide.size() >= 3 ? wide : ascii;
            if (text.size() >= 3) {
                std::fprintf(stderr, "[jit-code-watch pid=%d]   r%u -> %s\"%s\"\n",
                             static_cast<int>(getpid()), static_cast<unsigned>(reg),
                             &text == &wide ? "L" : "", text.c_str());
            }
        }
    }
    // Snapshot pointed-to values through guest-memory reads, never by
    // dereferencing a guest address as a host pointer. These are diagnostics,
    // so inaccessible argument addresses are reported, not guest faults.
    // Nonvolatile registers can retain the owning object across a guest call.
    // Snapshot every register uniformly, without guessing a guest ABI/layout.
    for (u8 reg = 0; reg < 16; ++reg) {
        const auto address = cpu->gpr.get64(reg);
        std::array<u64, 4> words{};
        bool accessible = address <= std::numeric_limits<u64>::max() - 31;
        if (accessible) {
            for (usize word = 0; word < words.size(); ++word) {
                const auto value = memory->read_u64(address + word * sizeof(u64));
                if (value.is_err()) { accessible = false; break; }
                words[word] = value.value();
            }
        }
        std::fprintf(stderr, "[jit-code-watch pid=%d] r%u memory",
                     static_cast<int>(getpid()), static_cast<unsigned>(reg));
        if (!accessible) { std::fprintf(stderr, " inaccessible\n"); continue; }
        for (const u64 word : words)
            std::fprintf(stderr, " %016llx", (unsigned long long)word);
        std::fprintf(stderr, "\n");
    }
    const auto sp = cpu->gpr.get64(4);
    static const u64 stack_words = [] {
        const char* value = std::getenv("VITREO_CODE_WATCH_STACK_WORDS");
        return value ? std::min<u64>(std::strtoull(value, nullptr, 0), 64) : 8;
    }();
    for (u64 slot = 0; slot < stack_words && sp <= std::numeric_limits<u64>::max() - slot * 8; ++slot) {
        const auto value = memory->read_u64(sp + slot * 8);
        if (value.is_ok()) std::fprintf(stderr, "[jit-code-watch pid=%d] stack[%llu]=0x%llx\n",
            static_cast<int>(getpid()), (unsigned long long)slot,
            (unsigned long long)value.value());
    }
    const auto argument = cpu->gpr.get64(1);
    // Heap addresses vary between runs. Let the diagnostic caller select a
    // bounded field window relative to the first argument, without embedding
    // a guest layout or changing which stores are recorded.
    static const u64 store_offset = [] {
        const char* value = std::getenv("VITREO_CODE_WATCH_STORE_OFFSET");
        return value ? std::strtoull(value, nullptr, 0) : 0;
    }();
    static const u64 store_bytes = [] {
        const char* value = std::getenv("VITREO_CODE_WATCH_STORE_BYTES");
        return value ? std::min<u64>(std::strtoull(value, nullptr, 0), 65536) : 32;
    }();
    if (argument <= std::numeric_limits<u64>::max() - store_offset) {
        const u64 lo = argument + store_offset;
        if (lo <= std::numeric_limits<u64>::max() - store_bytes)
            store_ring_dump(lo, lo + store_bytes, 16);
    }
}

} // namespace vitreo::cpu::jit
