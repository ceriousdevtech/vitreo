// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Lightweight write-watchpoint for diagnosing stuck spinlocks / atomics.
//
// Enabled only when the env var VITREO_WATCH_ADDR is set to a hex guest
// address. Every guest write that covers the watched address is logged with
// the value written and a short "who" tag (interpreter vs JIT path). This lets
// us see a lock byte's full history: whether it is born already-held, written
// once and never released, or toggled normally.

#pragma once

#include <unordered_set>
#include <atomic>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <limits>
#include <pthread.h>
#include <unistd.h>
#include <vector>

#include "vitreo/types.hpp"

namespace vitreo {

// Runtime-armed write watch. VITREO_WATCH_ADDR is fixed at process start;
// this address can be set later, once a guest address is discovered from PC
// watches.
inline std::atomic<u64> g_mem_watch_runtime_addr{0};
inline std::atomic<bool> g_mem_watch_force_interpreter{false};
inline std::atomic<int> g_mem_watch_env{-1};

inline std::atomic<u64>& mem_watch_runtime_addr() noexcept {
    return g_mem_watch_runtime_addr;
}
// When a runtime watch is armed, force interpreter execution so stores go
// through GuestMemory::write_* (JIT linear stores bypass mem_watch_check).
inline std::atomic<bool>& mem_watch_force_interpreter() noexcept {
    return g_mem_watch_force_interpreter;
}
inline void mem_watch_arm(u64 addr) noexcept {
    if (addr != 0) {
        mem_watch_runtime_addr().store(addr, std::memory_order_release);
        mem_watch_force_interpreter().store(true, std::memory_order_release);
        // Publish into the single gate word too, so the per-store fast path
        // is one relaxed load instead of one load per watch source. A runtime
        // arm never disarms, so this can only move -1/0 to 1.
        g_mem_watch_env.store(1, std::memory_order_release);
    }
}

// Current guest RIP, published by the interpreter so the write-watch can
// attribute a memory write to the instruction performing it.
inline u64& mem_watch_cur_rip() noexcept {
    static thread_local u64 rip = 0;
    return rip;
}

struct MemWatchRegisters {
    u32 eax{};
    u32 ecx{};
    u32 edx{};
    u32 ebx{};
    u32 esp{};
    u32 ebp{};
    u32 esi{};
    u32 edi{};
};

// Register state immediately before the interpreter executes the instruction
// attributed by mem_watch_cur_rip().  Keeping this beside the RIP makes a
// watched copy self-describing: the report identifies both the write and the
// guest source/destination state that produced it without a high-volume PC
// trace.  This is diagnostic state only and is populated only while a memory
// watch is active.
inline MemWatchRegisters& mem_watch_cur_registers() noexcept {
    static thread_local MemWatchRegisters registers{};
    return registers;
}

// Guest address of the most recently dispatched JIT block, published by
// JitCompiler::execute(). Fault diagnostics need this for the same reason the
// write-watch above needs mem_watch_cur_rip(): only the interpreter publishes a
// per-instruction RIP, so a branch taken by compiled code is otherwise blamed on
// whatever the interpreter last ran — typically a thunk, far from the real site.
// Block granularity is enough to name the guest function that branched.
inline u64& last_jit_block_pc() noexcept {
    static thread_local u64 pc = 0;
    return pc;
}

// VITREO_WATCH_ADDR accepts one address or a comma-separated list, so a single
// run can follow several related globals at once (e.g. a buffer pointer plus
// the width/height that size it) instead of one address per run.
inline const std::vector<u64>& mem_watch_addrs() noexcept {
    static const std::vector<u64> addrs = [] {
        std::vector<u64> v;
        const char* e = std::getenv("VITREO_WATCH_ADDR");
        if (e && e[0]) {
            const char* p = e;
            while (*p) {
                char* end = nullptr;
                u64 a = static_cast<u64>(std::strtoull(p, &end, 0));
                if (end == p) break;
                if (a != 0) v.push_back(a);
                p = (*end == ',') ? end + 1 : end;
            }
        }
        return v;
    }();
    return addrs;
}

/// Optional value watch: log the first N guest u32 stores equal to this pattern.
/// Set VITREO_WATCH_VALUE=0x46edb500 (e.g. float 30426.5) to find the writer.
/// Pair with VITREO_JIT_LINEAR=0 so stores hit jit_mem_write* (linear stores
/// bypass mem_watch_check).
inline u32 mem_watch_value_pattern() noexcept {
    static const u32 pattern = [] {
        const char* e = std::getenv("VITREO_WATCH_VALUE");
        if (!e || !e[0]) return 0u;
        return static_cast<u32>(std::strtoul(e, nullptr, 0));
    }();
    return pattern;
}

inline std::atomic<u32>& mem_watch_value_hits() noexcept {
    static std::atomic<u32> hits{0};
    return hits;
}

/// One-time parse of the env knobs. Out of line from the gate below so the
/// disabled path stays a load-and-branch; never inlined back into it.
[[gnu::noinline]] inline bool mem_watch_enabled_init() noexcept {
    const int env =
        (!mem_watch_addrs().empty() || mem_watch_value_pattern() != 0) ? 1 : 0;
    // Compare-exchange rather than store: mem_watch_arm() may have published a
    // runtime watch while this parse was running, and a plain store would
    // silently drop it. Losing the race means the armed value already stands.
    int expected = -1;
    if (!g_mem_watch_env.compare_exchange_strong(expected, env,
                                                 std::memory_order_relaxed)) {
        return expected != 0;
    }
    return env != 0;
}

/// Cheap gate for the per-write hot path: one relaxed load when off.
///
/// The runtime-armed watch used to be a second atomic load here, on every
/// guest store. mem_watch_arm() now folds itself into this same word, so both
/// sources are answered by one load.
inline bool mem_watch_enabled() noexcept {
    const int env = g_mem_watch_env.load(std::memory_order_relaxed);
    if (env >= 0) return env != 0;
    return mem_watch_enabled_init();
}

/// Back-compat: the first watched address, or 0 when watching is off.
inline u64 mem_watch_addr() noexcept {
    static const u64 first = mem_watch_addrs().empty() ? u64{0}
                                                       : mem_watch_addrs()[0];
    return first;
}

// --- Code-execution watch ---------------------------------------------------
// VITREO_CODE_WATCH is a comma-separated list of guest code addresses. When the
// interpreter is about to execute an instruction at one of them, the call is
// logged (return address + first stack args). This traces calls into a guest
// vendor DLL, including decode and advance entry points, without stubbing it.
inline const std::vector<u64>& code_watch_set() noexcept {
    static const std::vector<u64> set = [] {
        std::vector<u64> v;
        const char* e = std::getenv("VITREO_CODE_WATCH");
        if (e && e[0]) {
            const char* p = e;
            while (*p) {
                char* end = nullptr;
                u64 a = static_cast<u64>(std::strtoull(p, &end, 0));
                if (end == p) break;
                v.push_back(a);
                p = (*end == ',') ? end + 1 : end;
            }
        }
        return v;
    }();
    return set;
}

inline bool code_watch_enabled() noexcept { return !code_watch_set().empty(); }

inline bool code_watch_hit(u64 rip) noexcept {
    for (u64 a : code_watch_set()) if (a == rip) return true;
    return false;
}

// Host thread identity for a watch report. A watched global that is written by
// one thread and cleared by another looks identical to one thread toggling it
// unless the reports are thread-tagged, so every line carries this.
inline u64 mem_watch_host_tid() noexcept {
    static thread_local u64 tid = [] {
        u64 v = 0;
        pthread_threadid_np(nullptr, &v);
        return v;
    }();
    return tid;
}

// --- Fault-triggered store ring ---------------------------------------------
// VITREO_STORE_RING=N keeps the last N guest stores (address, value, size, RIP,
// thread) in a wrap-around buffer and dumps the ones near a faulting address
// when the process dies on an unhandled access violation.
//
// Why this exists: VITREO_WATCH_ADDR and PCTRACE memtaps must be aimed at an
// address *before* the run, which is useless when the interesting address is
// heap-random per run and only its offset is stable, as on a recorded
// browser-renderer fault. A ring answers the retrospective question instead:
// which store wrote the neighbours of this pointer, and where did that loop
// stop?
struct StoreRecord {
    u64 addr;
    u64 value;
    u64 rip;
    u64 tid;
    u32 size;
    const char* who;
};

// -1 until VITREO_STORE_RING has been read. Namespace scope rather than a
// function-local static inside store_ring_capacity(): the capacity is queried
// at the top of mem_watch_check() for EVERY guest store, and on Apple a
// function-local static pays a `_tlv_get_addr` call for its guard there.
inline std::atomic<long long> g_store_ring_capacity{-1};

[[gnu::noinline]] inline usize store_ring_capacity_init() noexcept {
    usize capacity = 0;
    const char* e = std::getenv("VITREO_STORE_RING");
    if (e && e[0]) {
        const unsigned long long n = std::strtoull(e, nullptr, 0);
        if (n != 0) {
            usize pow2 = 1;
            while (pow2 < n && pow2 < (usize{1} << 24)) pow2 <<= 1;
            capacity = pow2;
        }
    }
    g_store_ring_capacity.store(static_cast<long long>(capacity),
                                std::memory_order_relaxed);
    return capacity;
}

inline usize store_ring_capacity() noexcept {
    const long long capacity =
        g_store_ring_capacity.load(std::memory_order_relaxed);
    if (capacity >= 0) return static_cast<usize>(capacity);
    return store_ring_capacity_init();
}

inline bool store_ring_enabled() noexcept { return store_ring_capacity() != 0; }

inline std::vector<StoreRecord>& store_ring_buffer() noexcept {
    static std::vector<StoreRecord> buffer(store_ring_capacity());
    return buffer;
}

inline std::atomic<u64>& store_ring_cursor() noexcept {
    static std::atomic<u64> cursor{0};
    return cursor;
}

inline std::mutex& store_ring_mutex() noexcept {
    static std::mutex mutex;
    return mutex;
}

// Optional low-word filter for a timing-sensitive capture. Unlike a value
// watch, this leaves the memory model and cache configuration unchanged.
// VITREO_STORE_RING_MATCH32 is a comma-separated list of unsigned 32-bit
// values. Only stores at least four bytes wide with a matching low word are
// retained. An unset filter retains the complete history as before.
inline bool store_ring_matches(u64 value, usize size) noexcept {
    static const std::vector<u32> values = [] {
        std::vector<u32> result;
        const char* cursor = std::getenv("VITREO_STORE_RING_MATCH32");
        if (!cursor || !*cursor) return result;
        while (*cursor) {
            char* end = nullptr;
            const auto parsed = std::strtoull(cursor, &end, 0);
            if (end == cursor || parsed > std::numeric_limits<u32>::max() ||
                (*end && *end != ',')) {
                std::fprintf(stderr, "[vitreo] invalid VITREO_STORE_RING_MATCH32\n");
                std::abort();
            }
            result.push_back(static_cast<u32>(parsed));
            if (!*end) break;
            cursor = end + 1;
            if (!*cursor) {
                std::fprintf(stderr, "[vitreo] invalid VITREO_STORE_RING_MATCH32\n");
                std::abort();
            }
        }
        return result;
    }();
    if (values.empty()) return true;
    if (size < sizeof(u32)) return false;
    for (const u32 expected : values)
        if (static_cast<u32>(value) == expected) return true;
    return false;
}

// Optional repeating address interval: MASK,LO,HI, with MASK a contiguous
// low-bit mask and [LO,HI) inside that period. This captures every value and
// width near a heap offset without serializing unrelated zero stores.
struct StoreAddressFilter {
    u64 mask{0}, lo{0}, hi{0};
    bool enabled{false};

    bool matches(u64 address, usize size) const noexcept {
        if (!enabled) return true;
        if (size == 0) return false;
        const u64 offset = address & mask;
        if (offset < hi && (offset >= lo || lo - offset < size)) return true;
        // An access crossing the period boundary also touches its beginning.
        return size - 1 > mask - offset &&
               size - 1 - (mask - offset) > lo;
    }
};

inline const StoreAddressFilter& store_ring_address_filter() noexcept {
    static const StoreAddressFilter filter = [] {
        StoreAddressFilter result;
        const char* cursor = std::getenv("VITREO_STORE_RING_ADDRESS");
        if (!cursor || !*cursor) return result;
        u64* fields[] = {&result.mask, &result.lo, &result.hi};
        for (usize i = 0; i < 3; ++i) {
            char* end = nullptr;
            errno = 0;
            *fields[i] = std::strtoull(cursor, &end, 0);
            if (*cursor == '-' || end == cursor || errno == ERANGE ||
                (i < 2 ? *end != ',' : *end != '\0')) {
                std::fprintf(stderr, "[vitreo] invalid VITREO_STORE_RING_ADDRESS\n");
                std::abort();
            }
            cursor = end + (i < 2 ? 1 : 0);
        }
        if (result.mask == std::numeric_limits<u64>::max() ||
            (result.mask & (result.mask + 1)) != 0 ||
            result.lo >= result.hi || result.hi > result.mask + 1) {
            std::fprintf(stderr, "[vitreo] invalid VITREO_STORE_RING_ADDRESS interval\n");
            std::abort();
        }
        result.enabled = true;
        return result;
    }();
    return filter;
}

inline void store_ring_push(GuestAddr addr, usize size, u64 value,
                            const char* who, u64 instruction_rip = 0) noexcept {
    const usize capacity = store_ring_capacity();
    if (capacity == 0 || !store_ring_address_filter().matches(addr, size) ||
        !store_ring_matches(value, size)) return;
    std::lock_guard<std::mutex> lock(store_ring_mutex());
    auto& buffer = store_ring_buffer();
    const u64 slot =
        store_ring_cursor().fetch_add(1, std::memory_order_relaxed);
    u64 rip = instruction_rip ? instruction_rip : mem_watch_cur_rip();
    if (rip == 0) rip = last_jit_block_pc();
    buffer[slot & (capacity - 1)] =
        StoreRecord{static_cast<u64>(addr), value, rip, mem_watch_host_tid(),
                    static_cast<u32>(size), who};
}

inline bool store_record_overlaps(const StoreRecord& record, u64 lo, u64 hi) noexcept {
    if (lo >= hi || record.size == 0 || record.addr >= hi) return false;
    // Subtraction avoids overflowing the end of a high-address store.
    return record.addr >= lo || lo - record.addr < record.size;
}

/// Print, oldest first, the recorded stores overlapping [lo, hi).
inline void store_ring_dump(u64 lo, u64 hi, unsigned max_lines) noexcept {
    const usize capacity = store_ring_capacity();
    if (capacity == 0) return;
    std::lock_guard<std::mutex> lock(store_ring_mutex());
    auto& buffer = store_ring_buffer();
    const u64 total = store_ring_cursor().load(std::memory_order_relaxed);
    const u64 first = (total > capacity) ? (total - capacity) : 0;
    std::fprintf(stderr,
        "[store-ring pid=%d] stores in [0x%llx,0x%llx) — %llu retained, window %llu%s%s\n",
        static_cast<int>(getpid()),
        (unsigned long long)lo, (unsigned long long)hi,
        (unsigned long long)total, (unsigned long long)(total - first),
        std::getenv("VITREO_STORE_RING_MATCH32") ? " (value-filtered)" : "",
        store_ring_address_filter().enabled ? " (address-filtered)" : "");
    std::vector<const StoreRecord*> hits;
    for (u64 i = first; i < total; ++i) {
        const StoreRecord& record = buffer[i & (capacity - 1)];
        if (store_record_overlaps(record, lo, hi)) hits.push_back(&record);
    }
    const usize skip = (hits.size() > max_lines) ? hits.size() - max_lines : 0;
    if (skip != 0) {
        std::fprintf(stderr, "[store-ring]   (%zu earlier hits omitted)\n", skip);
    }
    for (usize i = skip; i < hits.size(); ++i) {
        const StoreRecord* record = hits[i];
        std::fprintf(stderr,
            "[store-ring pid=%d]   addr=0x%08llx size=%u value=0x%llx rip=0x%llx "
            "htid=%llu via=%s\n",
            static_cast<int>(getpid()),
            (unsigned long long)record->addr, record->size,
            (unsigned long long)record->value, (unsigned long long)record->rip,
            (unsigned long long)record->tid,
            record->who ? record->who : "?");
    }
    if (hits.empty()) {
        std::fprintf(stderr,
            "[store-ring]   no recorded store touched that window\n");
    }
}

// --- Read watch -------------------------------------------------------------
// VITREO_READ_WATCH=LO-HI[,LO-HI...] (hex, half-open) logs each distinct
// (rip, address) guest DATA read that overlaps a range -- which fields of a
// structure (PEB, TEB, KUSER_SHARED_DATA, a module) some code consults. Only
// checked reads are seen; pair it with VITREO_JIT_SKIP_RANGE so the code of
// interest is interpreted. Bounded to 4096 reports.
struct ReadWatchRange { u64 lo; u64 hi; };
inline const std::vector<ReadWatchRange>& read_watch_ranges() noexcept {
    static const std::vector<ReadWatchRange> ranges = [] {
        std::vector<ReadWatchRange> out;
        const char* cursor = std::getenv("VITREO_READ_WATCH");
        while (cursor && *cursor) {
            char* end = nullptr;
            const u64 lo = std::strtoull(cursor, &end, 16);
            if (!end || *end != '-') break;
            const u64 hi = std::strtoull(end + 1, &end, 16);
            out.push_back({lo, hi});
            cursor = (end && *end == ',') ? end + 1 : nullptr;
        }
        return out;
    }();
    return ranges;
}
inline bool read_watch_enabled() noexcept {
    static const bool enabled = !read_watch_ranges().empty();
    return enabled;
}
inline u64& mem_watch_cur_rip() noexcept;
[[gnu::noinline]] inline void read_watch_check(u64 addr, usize size, u64 value) noexcept {
    for (const auto& r : read_watch_ranges()) {
        if (addr < r.hi && addr + size > r.lo) {
            static std::mutex mutex;
            static std::unordered_set<u64> seen;
            static u32 reports = 0;
            const u64 rip = mem_watch_cur_rip();
            std::lock_guard<std::mutex> lock(mutex);
            if (reports >= 4096 || !seen.insert((rip << 20) ^ addr ^ (u64{size} << 60)).second) return;
            ++reports;
            std::fprintf(stderr, "[read-watch pid=%d] addr=0x%llx size=%zu value=0x%llx rip=0x%llx\n",
                         (int)getpid(), (unsigned long long)addr, size,
                         (unsigned long long)value, (unsigned long long)rip);
            return;
        }
    }
}

// Report a write if it overlaps the watched address. `value` is the low bytes
// of the value written (enough for a lock byte/word).
inline void mem_watch_check(GuestAddr addr, usize size, u64 value, const char* who) noexcept {
    store_ring_push(addr, size, value, who);
    if (!mem_watch_enabled()) return;
    auto report = [&](u64 w) {
        if (addr <= w && w < addr + size) {
            const auto& r = mem_watch_cur_registers();
            std::fprintf(stderr,
                "[watch pid=%d htid=%llu] WRITE addr=0x%llx size=%zu value=0x%llx via=%s rip=0x%llx "
                "eax=0x%x ecx=0x%x edx=0x%x ebx=0x%x esp=0x%x ebp=0x%x esi=0x%x edi=0x%x\n",
                (int)getpid(), (unsigned long long)mem_watch_host_tid(),
                (unsigned long long)addr, size,
                (unsigned long long)value, who,
                (unsigned long long)mem_watch_cur_rip(),
                r.eax, r.ecx, r.edx, r.ebx, r.esp, r.ebp, r.esi, r.edi);
        }
    };
    for (u64 w : mem_watch_addrs()) {
        report(w);
    }
    const u64 runtime = mem_watch_runtime_addr().load(std::memory_order_acquire);
    if (runtime != 0) {
        report(runtime);
    }
    const u32 pattern = mem_watch_value_pattern();
    if (pattern != 0 && size >= 4 &&
        static_cast<u32>(value) == pattern &&
        mem_watch_value_hits().fetch_add(1, std::memory_order_relaxed) < 8u) {
        const auto& r = mem_watch_cur_registers();
        const u64 rip = mem_watch_cur_rip() != 0 ? mem_watch_cur_rip()
                                                : last_jit_block_pc();
        std::fprintf(stderr,
            "[watch-value pid=%d htid=%llu] WRITE addr=0x%llx size=%zu "
            "value=0x%08x via=%s rip=0x%llx jit_block=0x%llx "
            "eax=0x%x ecx=0x%x edx=0x%x ebx=0x%x esp=0x%x ebp=0x%x "
            "esi=0x%x edi=0x%x\n",
            (int)getpid(), (unsigned long long)mem_watch_host_tid(),
            (unsigned long long)addr, size, pattern, who,
            (unsigned long long)rip,
            (unsigned long long)last_jit_block_pc(), r.eax, r.ecx, r.edx,
            r.ebx, r.esp, r.ebp, r.esi, r.edi);
        std::fflush(stderr);
    }
}

// Observe each byte of a bulk write, rather than labelling an arbitrary-size
// write with only buffer[0]. Eight-byte records preserve the value filter and
// let a retrospectively chosen interior address find its actual writer.
inline void mem_watch_bytes(GuestAddr addr, const u8* bytes, usize size,
                            const char* who) noexcept {
    for (usize offset = 0; offset < size;) {
        const usize count = std::min<usize>(sizeof(u64), size - offset);
        u64 value = 0;
        std::memcpy(&value, bytes + offset, count);
        mem_watch_check(addr + offset, count, value, who);
        offset += count;
    }
}

}  // namespace vitreo
