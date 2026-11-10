// SPDX-License-Identifier: LGPL-2.1-only
// WS2: microbenchmark, crossover rule and gate.
//
// LoopIdiomBench.Gate runs only with VITREO_BENCH_WS2=1, with no build or
// ctest alongside. Phase 1 (crossover) times each (pattern, size) with the
// prologue forced on and no threshold (OnNoThreshold) against it compiled
// out (Off). Each arm has its own JitCompiler and is compiled once. The arms
// are interleaved for 11 batches; the median and the best batch per arm are
// reported. Phase 2 (gate) repeats 4 B, 16 B, 1 KiB, 4 KiB and 64 KiB, plus
// the shipped crossover T = min_iters * element bytes and T/2, in the
// shipping configuration (Force::On: lowering policy, measured thresholds,
// inline gate).
//
// Gate per pattern (keep rule): a lowered shape has Off/On >= 1.2 at every
// gate size >= T, and On/Off <= 1.02 below T (T/2) and at 4 B and 16 B for
// every shape. Crossover (program rule, reported as RESULT): the smallest
// sampled size from which On is >= 10% faster at that size and at every
// larger sampled size; the shipped T is the smallest sampled size from which
// the best batch is >= 1.2x everywhere above. The measured table and the
// baked constants are in src/core/cpu/jit/loop_idiom_thresholds.hpp.

#include "test_jit_loop_idiom_fixture.hpp"

#include "core/cpu/jit/loop_idiom_mem.hpp"
#include "core/cpu/jit/loop_idiom_trip.hpp"
#include "core/cpu/jit/loop_idiom_runtime.hpp"
#include "core/memory/mem_watch.hpp"
#include "core/cpu/jit/loop_idiom_thresholds.hpp"

#include <pthread.h>
#include <sys/qos.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::loop_idiom;
using namespace vitreo::cpu::jit::loop_idiom::test;
using namespace vitreo::cpu::jit::loop_idiom::test::catalogue;

namespace {

struct BenchRow {
    u64 bytes;
    double on_ns;
    double off_ns;
};

u64 crossover_bytes(std::span<const BenchRow> rows) {
    u64 result = std::numeric_limits<u64>::max();
    for (usize i = rows.size(); i-- > 0;) {
        if (rows[i].off_ns / rows[i].on_ns >= 1.10) result = rows[i].bytes;
        else break;
    }
    return result;
}

constexpr u8 EAX = 0, ECX = 1, EDX = 2, ESI = 6, EDI = 7;
constexpr u64 SRC = 0x20100;
constexpr u64 DST = 0x40000;

struct BenchPattern {
    const char* name;
    std::span<const u8> code;
    bool is64;
    Kind kind;
    u32 elem;
    /// Memory for an n-element run (terminators etc.).
    std::function<void(GuestMemory&, u64 n)> mem;
    std::function<void(CpuState&, u64 n)> regs;
};

void plain_mem(GuestMemory& m, u64) { default_data(m); }

std::vector<BenchPattern> patterns() {
    auto zero_at = [](u64 off, u32 w) {
        return [off, w](GuestMemory& m, u64 n) {
            default_data(m);
            std::vector<u8> z(w, 0);
            ASSERT_TRUE(m.write_bytes(SRC + off * n, z.data(), z.size()));
        };
    };
    auto equal = [](GuestMemory& m, u64) {
        default_data(m);
        std::vector<u8> buf(0x10100, 0x5C);
        ASSERT_TRUE(m.write_bytes(SRC, buf.data(), buf.size()));
        ASSERT_TRUE(m.write_bytes(DST, buf.data(), buf.size()));
    };
    return {
        {"F1", kF1, false, Kind::Fill, 1, plain_mem, [](CpuState& c, u64 n) {
             c.gpr.set32(EDI, DST); c.gpr.set32(ECX, static_cast<u32>(n)); c.gpr.set32(EAX, 7); }},
        {"F3", kF3, false, Kind::Fill, 2, plain_mem, [](CpuState& c, u64 n) {
             c.gpr.set32(EDI, static_cast<u32>(DST + 2 * n)); c.gpr.set32(ECX, static_cast<u32>(0 - n)); }},
        {"F2", kF2, false, Kind::Fill, 4, plain_mem, [](CpuState& c, u64 n) {
             c.gpr.set32(EDI, DST); c.gpr.set32(EAX, 0); c.gpr.set32(ECX, static_cast<u32>(n));
             c.gpr.set32(EDX, 0x11223344); }},
        {"F4", kF4, true, Kind::Fill, 1, plain_mem, [](CpuState& c, u64 n) {
             c.gpr.set64(EDI, DST); c.gpr.set64(EAX, 0); c.gpr.set64(EDX, n); c.gpr.set64(ECX, 7); }},
        {"F5", kF5, true, Kind::Fill, 8, plain_mem, [](CpuState& c, u64 n) {
             c.gpr.set64(EDI, DST); c.gpr.set64(EAX, 0); c.gpr.set64(EDX, n); c.gpr.set64(ESI, 7); }},
        {"F6", kF6, true, Kind::Fill, 16, plain_mem, [](CpuState& c, u64 n) {
             c.gpr.set64(EDI, DST); c.gpr.set64(EAX, 0); c.gpr.set64(EDX, 16 * n); }},
        {"C1", kC1, false, Kind::Copy, 1, plain_mem, [](CpuState& c, u64 n) {
             c.gpr.set32(ESI, SRC); c.gpr.set32(EDI, DST); c.gpr.set32(ECX, static_cast<u32>(n)); }},
        {"C3", kC3, false, Kind::Copy, 4, plain_mem, [](CpuState& c, u64 n) {
             c.gpr.set32(ESI, SRC); c.gpr.set32(EDI, DST); c.gpr.set32(EAX, 0); c.gpr.set32(ECX, static_cast<u32>(n)); }},
        {"C7", kC7, true, Kind::Copy, 1, plain_mem, [](CpuState& c, u64 n) {
             c.gpr.set64(ESI, SRC + n); c.gpr.set64(EDI, DST + n); c.gpr.set64(ECX, 0 - n); }},
        {"C8", kC8, true, Kind::Copy, 8, plain_mem, [](CpuState& c, u64 n) {
             c.gpr.set64(ESI, SRC); c.gpr.set64(EDI, DST); c.gpr.set64(ECX, 0); c.gpr.set64(EDX, n); }},
        {"C9", kC9, true, Kind::Copy, 16, plain_mem, [](CpuState& c, u64 n) {
             c.gpr.set64(ESI, SRC); c.gpr.set64(EDI, DST); c.gpr.set64(EAX, 0); c.gpr.set64(EDX, 16 * n); }},
        {"S4", kS4, true, Kind::Scan, 1, zero_at(1, 1), [](CpuState& c, u64) {
             c.gpr.set64(ECX, SRC); c.gpr.set64(EAX, ~u64{0}); }},
        {"S5", kS5, true, Kind::Scan, 2, zero_at(2, 2), [](CpuState& c, u64) {
             c.gpr.set64(EAX, SRC); c.gpr.set64(ECX, ~u64{0}); }},
        {"S6", kS6, true, Kind::Scan, 16,
         [](GuestMemory& m, u64 n) {
             default_data(m);
             const u8 z = 0;
             ASSERT_TRUE(m.write_bytes(SRC + 16 * n - 1, &z, 1));
         },
         [](CpuState& c, u64) {
             c.gpr.set64(EAX, SRC);
             for (u8 i = 0; i < 16; ++i) c.sse.xmm[0].u8_[i] = 0;
         }},
        {"S7", kS7, true, Kind::Scan, 1, plain_mem, [](CpuState& c, u64 n) {
             c.gpr.set64(EDI, SRC); c.gpr.set64(EDX, n); c.gpr.set64(ESI, 0x01); }},
        {"M3", kM3, true, Kind::Compare, 1, equal, [](CpuState& c, u64 n) {
             c.gpr.set64(EDI, SRC); c.gpr.set64(ESI, DST); c.gpr.set64(ECX, 0); c.gpr.set64(EDX, n); }},
        {"M4", kM4, true, Kind::Compare, 8, equal, [](CpuState& c, u64 n) {
             c.gpr.set64(EDI, SRC); c.gpr.set64(ESI, DST); c.gpr.set64(ECX, 0); c.gpr.set64(EDX, n); }},
    };
}

/// One arm: its own memory and compiler, the block compiled once.
struct Arm {
    std::unique_ptr<GuestMemory> mem;
    std::unique_ptr<JitCompiler> jit;
    BasicBlock block;
    CompiledBlock* cb{nullptr};
    CpuState init{};

    CompiledBlock head{};
    /// Blocks at RIPs inside the loop, compiled on first use.
    std::vector<std::pair<u64, CompiledBlock>> inner;
    u64 end{0};

    Arm(const BenchPattern& p, u64 n, Force f) {
        ScopedForce s(f);
        mem = make_memory(p.code, [&](GuestMemory& m) { p.mem(m, n); });
        jit = std::make_unique<JitCompiler>(*mem);
        block = decode_block(p.code, p.is64);
        cb = jit->compile(block);
        head = *cb;
        end = kCodeBase + p.code.size();
        code = p.code;
        is64 = p.is64;
        init_cpu(init, p.is64);
        p.regs(init, n);
    }

    std::span<const u8> code;
    bool is64{false};

    /// The in-block loop cap can leave RIP at the back-edge Jcc; compile a
    /// block there on first use (once per arm, like the dispatcher would).
    CompiledBlock* block_at(u64 rip) {
        if (rip == kCodeBase) return &head;
        for (auto& [r, b] : inner) if (r == rip) return &b;
        const BasicBlock b = decode_block(code.subspan(rip - kCodeBase), is64, rip);
        CompiledBlock* c = jit->compile(b);
        if (c == nullptr) return nullptr;
        inner.emplace_back(rip, *c);
        return &inner.back().second;
    }

    void call() {
        CpuState c = init;
        for (u32 guard = 0; guard < 100000 && c.rip >= kCodeBase && c.rip < end &&
                            !c.pending_raise.active; ++guard) {
            CompiledBlock* b = block_at(c.rip);
            if (b == nullptr) break;
            (void)vitreo::cpu::jit::test::execute_like_dispatcher(*jit, *b, c);
            ++entries;
        }
    }
    u64 entries{0};

    double ns_per_call(u64 reps) {
        const auto t0 = std::chrono::steady_clock::now();
        for (u64 i = 0; i < reps; ++i) call();
        const auto t1 = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(reps);
    }
};

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

}  // namespace

TEST(LoopIdiomBench, CrossoverRule) {
    const BenchRow mono[] = {{4, 10, 9}, {16, 10, 10.5}, {64, 10, 12}, {256, 10, 30}};
    EXPECT_EQ(crossover_bytes(mono), 64u);
    const BenchRow dip[] = {{4, 10, 12}, {16, 10, 10.5}, {64, 10, 12}, {256, 10, 30}};
    EXPECT_EQ(crossover_bytes(dip), 64u) << "the crossover moves past a dip";
    const BenchRow never[] = {{4, 10, 9}, {16, 10, 10}, {64, 10, 10.9}};
    EXPECT_EQ(crossover_bytes(never), std::numeric_limits<u64>::max());
    const BenchRow all[] = {{4, 10, 11}, {16, 10, 20}};
    EXPECT_EQ(crossover_bytes(all), 4u);
}

namespace {

struct Measured {
    double on_ns, off_ns;          ///< medians
    double on_min_ns, off_min_ns;  ///< best batches (interference-free floor)
};

/// Interleaved reps per arm. The program rule is a median of >= 5; 11 short
/// batches keep the median stable on a machine that is not fully idle.
constexpr int kReps = 11;

/// One (pattern, size): `on` compiled and run under `on_force`, `off` under
/// Off; kReps interleaved batches of >= 2 ms, median and min per arm.
Measured measure(const BenchPattern& p, u64 n, Force on_force) {
    Arm on(p, n, on_force);
    Arm off(p, n, Force::Off);
    ScopedForce run(on_force);
    on.call();  // warm: compile any in-loop block before timing
    off.call();
    u64 reps = 16;
    while (off.ns_per_call(reps) * static_cast<double>(reps) < 2e6 && reps < (1u << 24)) {
        reps *= 2;
    }
    std::vector<double> t_on, t_off;
    for (int r = 0; r < kReps; ++r) {
        t_on.push_back(on.ns_per_call(reps));
        t_off.push_back(off.ns_per_call(reps));
    }
    return {median(t_on), median(t_off), *std::min_element(t_on.begin(), t_on.end()),
            *std::min_element(t_off.begin(), t_off.end())};
}

}  // namespace

TEST(LoopIdiomBench, Gate) {
    const char* env = std::getenv("VITREO_BENCH_WS2");
    if (env == nullptr || env[0] != '1') GTEST_SKIP() << "set VITREO_BENCH_WS2=1 on an idle machine";
    const u64 sizes[] = {4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 16384, 65536};
    // Keep the arms on performance cores: an efficiency-core batch in one
    // arm and not the other would read as a 5-10x "speedup".
    (void)pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    const char* only = std::getenv("VITREO_BENCH_WS2_ONLY");
    for (const BenchPattern& p : patterns()) {
        if (only != nullptr && std::string(only) != p.name) continue;
        const u8 lg = static_cast<u8>(__builtin_ctz(p.elem));
        const auto mr = match_bytes(p.code, p.is64);
        ASSERT_TRUE(mr.match) << p.name;
        const bool gated = mr.match->gate.form != InlineGate::Form::None;
        const bool is_lowered = lowered(p.kind, lg, gated);
        // Phase 1 (crossover): the prologue forced on with no threshold.
        std::vector<BenchRow> rows;
        std::printf("\n%-4s %7s %10s %10s %7s   (measure: forced, no threshold)\n", p.name, "bytes",
                    "on_ns", "off_ns", "off/on");
        // VITREO_BENCH_WS2_GATE_ONLY=1 skips phase 1 (re-checking the gate).
        const bool gate_only = std::getenv("VITREO_BENCH_WS2_GATE_ONLY") != nullptr;
        for (u64 bytes : sizes) {
            if (bytes < p.elem || gate_only) continue;
            const Measured m = measure(p, bytes / p.elem, Force::OnNoThreshold);
            rows.push_back({bytes, m.on_ns, m.off_ns});
            std::printf("%-4s %7llu %10.1f %10.1f %7.2f   min %10.1f %10.1f %7.2f\n", p.name,
                        static_cast<unsigned long long>(bytes), m.on_ns, m.off_ns,
                        m.off_ns / m.on_ns, m.on_min_ns, m.off_min_ns,
                        m.off_min_ns / m.on_min_ns);
        }
        const u64 x = crossover_bytes(rows);
        std::printf("%-4s RESULT gated=%d crossover=%lld B (%lld iterations) lowered=%d min_iters=%u\n",
                    p.name, gated ? 1 : 0,
                    x == std::numeric_limits<u64>::max() ? -1LL : static_cast<long long>(x),
                    x == std::numeric_limits<u64>::max() ? -1LL : static_cast<long long>(x / p.elem),
                    is_lowered ? 1 : 0, min_iters(p.kind, lg));
        // Phase 2 (gate): shipping configuration (policy + measured
        // thresholds + inline gate) vs Off. Keep rule: a lowered pattern is
        // >= 1.2x faster at and above its shipped crossover (min_iters
        // elements) and no slower anywhere below it.
        const u64 thr = is_lowered ? u64{min_iters(p.kind, lg)} * p.elem : 0;
        std::vector<u64> gate_sizes = {4, 16, 1024, 4096, 65536};
        if (is_lowered) {
            gate_sizes.push_back(thr);
            if (thr / 2 > 16) gate_sizes.push_back(thr / 2);
        }
        std::sort(gate_sizes.begin(), gate_sizes.end());
        gate_sizes.erase(std::unique(gate_sizes.begin(), gate_sizes.end()), gate_sizes.end());
        for (u64 bytes : gate_sizes) {
            if (bytes < p.elem) continue;
            const Measured m = measure(p, bytes / p.elem, Force::On);
            std::printf("%-4s GATE %7llu on=%.1f off=%.1f off/on=%.2f min_on=%.1f min_off=%.1f%s\n",
                        p.name, static_cast<unsigned long long>(bytes), m.on_ns, m.off_ns,
                        m.off_ns / m.on_ns, m.on_min_ns, m.off_min_ns,
                        is_lowered && bytes == thr ? "  (crossover)" : "");
            // Judged on the best batch per arm (see loop_idiom_thresholds.hpp:
            // on a machine that is not idle the median measures preemption).
            if (bytes <= 16 || (is_lowered && bytes < thr)) {
                EXPECT_LE(m.on_min_ns / m.off_min_ns, 1.02) << p.name << " at " << bytes << " B";
            }
            if (is_lowered && bytes >= thr) {
                EXPECT_GE(m.off_min_ns / m.on_min_ns, 1.2) << p.name << " at " << bytes << " B";
            }
        }
        std::fflush(stdout);
    }
}

TEST(LoopIdiomBench, HelperFixedCost) {
    // Direct call cost of jit_loop_idiom for a 4-iteration byte fill, with
    // no generated code around it: separates the helper's fixed cost from
    // the prologue and dispatcher costs measured by Gate.
    const char* env = std::getenv("VITREO_BENCH_WS2");
    if (env == nullptr || env[0] != '1') GTEST_SKIP() << "set VITREO_BENCH_WS2=1 on an idle machine";
    GuestMemory mem;
    ASSERT_TRUE(mem.map_region(0x20000, 0x10000, MemoryProtection::ReadWrite));
    const auto r = match_bytes(kF1, false);
    ASSERT_TRUE(r.match);
    const Words w = *pack(r.match->desc);
    CpuState init;
    init.init_protected_mode();
    init.rip = kCodeBase;
    init.gpr.set32(7, 0x20000);
    init.gpr.set32(1, 4);
    ScopedForce f(Force::On);
    constexpr u64 kReps = 200000;
    for (int rep = 0; rep < 3; ++rep) {
        const auto t0 = std::chrono::steady_clock::now();
        CpuState c = init;
        for (u64 i = 0; i < kReps; ++i) {
            c.gpr.set32(7, 0x20000);
            c.gpr.set32(1, 4);
            (void)jit_loop_idiom(&c, &mem, w[0], w[1], w[2], w[3], w[4], w[5]);
        }
        const auto t1 = std::chrono::steady_clock::now();
        std::printf("helper F1 n=4: %.1f ns/call\n",
                    std::chrono::duration<double, std::nano>(t1 - t0).count() / kReps);
    }
    auto time = [&](const char* what, auto&& fn) {
        const auto t0 = std::chrono::steady_clock::now();
        for (u64 i = 0; i < kReps; ++i) fn();
        const auto t1 = std::chrono::steady_clock::now();
        std::printf("  %-22s %.1f ns\n", what,
                    std::chrono::duration<double, std::nano>(t1 - t0).count() / kReps);
    };
    volatile u64 sink = 0;
    std::printf("  sizeof(CpuState)       %zu\n", sizeof(CpuState));
    time("unpack", [&] { sink = sink + unpack(w)->elem_log2; });
    time("probe_access", [&] { sink = sink + mem.probe_access(0x20000, 1).readable; });
    time("safe_span store", [&] { sink = sink + safe_span(mem, 0x20000, false, 1, 4, true, false).elems; });
    time("host_ptr_store", [&] { sink = sink + (mem.host_ptr_store(0x20000, 2) != nullptr); });
    time("mem_watch_check", [&] { vitreo::mem_watch_check(0x20000, 2, 0, "x"); });
    time("code_alias", [&] { sink = sink + mem.overlaps_code_alias(0x20000, 4); });
    time("jit_code_pages", [&] { sink = sink + mem.jit_code_pages_overlap(0x20000, 4); });
    time("trip", [&] {
        sink = sink + first_induction_exit(Cond::NZ, true, 3, -1, 32, 0, max_iters_per_call(1)).k;
    });
    u8* host = mem.host_ptr_store(0x20000, 16);
    const u8 v[16] = {7};
    time("bulk_fill", [&] { bulk_fill(host, false, 1, 2, v, false); });
    time("full helper", [&] {
        CpuState& c = init;
        c.gpr.set32(7, 0x20000);
        c.gpr.set32(1, 4);
        sink = sink + jit_loop_idiom(&c, &mem, w[0], w[1], w[2], w[3], w[4], w[5]);
    });
}
