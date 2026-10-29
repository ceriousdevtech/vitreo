// Synthetic code-arena churn: several dispatcher-shaped threads compile far
// more code than the arena budget holds while re-executing a hot working set.
//
// What is measured, per process, at the end of the run:
//   refused        native entries refused because the arena gate was closed
//                  (JitCodeArena::arena_gate_refusals) plus dispatch-chain
//                  leases this test could not take — each one is a block the
//                  real dispatcher would have interpreted instead.
//   retirements    arena retirements (JitCodeArena::arena_reclaim_count).
//   deferred       compiles that returned null with compile_deferred() set;
//                  max_streak is the longest run of consecutive deferrals on
//                  one thread, i.e. how long a thread went without being able
//                  to land any new code.
//   wrong          executions whose result did not match the block — the one
//                  number that must be exactly zero: it means recycled bytes
//                  were entered.
//
// The arena is created once per process at the first JitCompiler, so the
// budget is shrunk from a static initializer, before gtest runs anything.
// VITREO_JIT_ARENA_CHUNK_MB shapes the chunked arena (1 MB chunks, 16 of
// them). VITREO_JIT_ARENA_SEGMENT_MB is the pre-chunk slot ring's knob; it is
// still set so the same source reports the old model when built against it.
//
// MEASURED (6 workers, 16 MB budget, 96 B blocks, ~350k compiles):
//   slot ring (512 MB-shaped, 4 x 4 MB here): refused=353,178 native entries
//     (gate=176,589 + chain leases=176,589), retirements=8, window=YES.
//   chunked arena (16 x 1 MB): refused=0, gate never closed, chunk
//     retirements=35, wrong=0, window=no.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/memory/guest_memory.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

namespace {

const bool g_env_configured = [] {
    setenv("VITREO_JIT_ARENA_MB", "16", 0);
    setenv("VITREO_JIT_ARENA_SEGMENT_MB", "4", 0);
    setenv("VITREO_JIT_ARENA_CHUNK_MB", "1", 0);
    return true;
}();

constexpr u8 EAX = static_cast<u8>(RegIndex::RAX);
constexpr u8 ESP = static_cast<u8>(RegIndex::RSP);
constexpr GuestAddr kCodeBase = 0x10000;
constexpr usize kCodeRegion = 0x400000;
constexpr int kWorkers = 6;
constexpr usize kStride = 8;
constexpr usize kHotSet = 128;

GuestAddr block_addr(int worker, usize index, usize per_worker) {
    return kCodeBase + (static_cast<usize>(worker) * per_worker + index) * kStride;
}

u32 block_value(int worker, usize index) {
    return 0x9E3779B9u * (static_cast<u32>(worker) + 1u) ^
           static_cast<u32>(index * 0x85EBCA6Bu) ^ 0x5A5A0000u;
}

BasicBlock decode_mov(GuestAddr addr, u32 imm) {
    u8 code[5] = {0xB8, static_cast<u8>(imm), static_cast<u8>(imm >> 8),
                  static_cast<u8>(imm >> 16), static_cast<u8>(imm >> 24)};
    BasicBlock block;
    block.start_addr = addr;
    MemoryByteReader reader(code, sizeof(code));
    DecodedInstruction inst;
    if (decode_instruction(reader, DecodeMode::Mode32, inst) ==
        DecodeError::Success) {
        block.instructions.push_back(inst);
    }
    block.byte_length = static_cast<u32>(reader.position());
    block.instruction_count = static_cast<u32>(block.instructions.size());
    return block;
}

struct WorkerStats {
    u64 compiled{0};
    u64 deferred{0};
    u64 failed{0};
    u64 max_streak{0};
    u64 executed{0};
    u64 stale{0};
    u64 wrong{0};
    u64 lease_refused{0};
    u64 hot_recompiles{0};
};

struct Run {
    std::unique_ptr<GuestMemory> memory;
    usize per_worker{0};
    std::vector<WorkerStats> stats;
    u64 refusals{0};
    u64 retirements{0};
    u64 abandons{0};
    double seconds{0};
    bool parked_redispatched{false};
};

// One dispatcher-shaped guest thread: a safepoint, then a short chain under a
// StableExecution lease in which it lands one new block and re-executes one
// hot block, exactly as the run loop interleaves compiles with execution.
void worker_main(Run& run, int worker, std::atomic<bool>& stop,
                 WorkerStats& out) {
    BlockCache cache(4096);
    JitCompiler jit(*run.memory);
    jit.set_block_cache(&cache);
    CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.set32(ESP, 0x10000);
    u64 streak = 0;

    auto execute_entry = [&](BasicBlock& entry, u32 expected) -> bool {
        if (entry.compiled_code == nullptr) return false;
        CompiledBlock cb = compiled_from_cache(entry);
        cpu.rip = entry.start_addr;
        cpu.gpr.set32(EAX, 0);
        const JitExecResult result = jit.execute(cb, cpu);
        if (result == JitExecResult::StaleCode) {
            ++out.stale;
            (void)jit.synchronize_arena_generation();
            return false;
        }
        ++out.executed;
        if (cpu.gpr.get32(EAX) != expected) ++out.wrong;
        return true;
    };

    for (usize i = 0; i < run.per_worker && !stop.load(std::memory_order_relaxed);
         ++i) {
        // Dispatcher safe point between chains.
        (void)JitCodeArena::reclaim_arena_if_requested();
        (void)jit.synchronize_arena_generation();

        JitCodeArena::ArenaReadLease chain{
            JitCodeArena::ArenaReadLease::StableExecution{}};
        if (!chain) {
            ++out.lease_refused;
            (void)jit.synchronize_arena_generation();
            continue;
        }

        const GuestAddr addr = block_addr(worker, i, run.per_worker);
        const u32 value = block_value(worker, i);
        BasicBlock* entry = cache.lookup(addr);
        if (entry == nullptr) entry = cache.insert(decode_mov(addr, value));
        CompiledBlock* compiled = jit.compile(*entry);
        if (compiled == nullptr) {
            if (jit.compile_deferred()) {
                ++out.deferred;
                out.max_streak = std::max(out.max_streak, ++streak);
            } else {
                ++out.failed;
            }
        } else {
            streak = 0;
            ++out.compiled;
            jit.publish_compiled_block(*entry, *compiled);
            (void)execute_entry(*entry, value);
        }

        // Hot working set: the first kHotSet blocks of this worker stay live
        // for the whole run, so every retirement of the code they sit in is
        // observed as StaleCode followed by a recompile.
        if (i >= kHotSet) {
            const usize hot = i % kHotSet;
            const GuestAddr hot_addr = block_addr(worker, hot, run.per_worker);
            const u32 hot_value = block_value(worker, hot);
            BasicBlock* hot_entry = cache.lookup(hot_addr);
            if (hot_entry == nullptr) {
                hot_entry = cache.insert(decode_mov(hot_addr, hot_value));
            }
            if (hot_entry->compiled_code == nullptr) {
                if (CompiledBlock* re = jit.compile(*hot_entry)) {
                    ++out.hot_recompiles;
                    jit.publish_compiled_block(*hot_entry, *re);
                } else if (jit.compile_deferred()) {
                    ++out.deferred;
                } else {
                    ++out.failed;
                }
                hot_entry = cache.lookup(hot_addr);
            }
            if (hot_entry != nullptr) (void)execute_entry(*hot_entry, hot_value);
        }
    }
}

usize calibrate_block_bytes(GuestMemory& memory) {
    BlockCache cache(16);
    JitCompiler jit(memory);
    jit.set_block_cache(&cache);
    const usize before = JitCodeArena::arena_bytes_used();
    BasicBlock* entry = cache.insert(decode_mov(kCodeBase, 0x11223344u));
    CompiledBlock* compiled = jit.compile(*entry);
    if (compiled == nullptr) return 0;
    return JitCodeArena::arena_bytes_used() - before;
}

}  // namespace

TEST(JitArenaChurn, ManyThreadsCompilingPastTheBudgetNeverEnterRecycledBytes) {
    ASSERT_TRUE(g_env_configured);
    struct Run run;
    run.memory = std::make_unique<GuestMemory>();
    ASSERT_TRUE(run.memory->map_region(kCodeBase, kCodeRegion,
                                       MemoryProtection::ReadWrite));

    const usize block_bytes = calibrate_block_bytes(*run.memory);
    ASSERT_GT(block_bytes, 0u) << "calibration compile failed";
    const usize budget = JitCodeArena::arena_budget();
    ASSERT_GT(budget, 0u);
    // Land about four budgets' worth of code across all workers.
    run.per_worker = std::min<usize>(
        (4 * budget) / (block_bytes * kWorkers),
        kCodeRegion / (kStride * kWorkers));
    ASSERT_GE(run.per_worker, kHotSet * 4);
    for (int w = 0; w < kWorkers; ++w) {
        for (usize i = 0; i < run.per_worker; ++i) {
            const u32 value = block_value(w, i);
            const u8 code[5] = {0xB8, static_cast<u8>(value),
                                static_cast<u8>(value >> 8),
                                static_cast<u8>(value >> 16),
                                static_cast<u8>(value >> 24)};
            ASSERT_TRUE(run.memory->write_bytes(
                block_addr(w, i, run.per_worker), code, sizeof(code)));
        }
    }

    const u64 refusals_before = JitCodeArena::arena_gate_refusals();
    const u64 retirements_before = JitCodeArena::arena_reclaim_count();
    const u64 abandons_before = JitCodeArena::arena_gate_abandons();

    run.stats.resize(kWorkers);
    std::atomic<bool> stop{false};
    std::atomic<bool> parked_ready{false};
    std::atomic<bool> parked_resume{false};

    // A guest thread blocked in a wait for the whole run: it holds a chain
    // lease that is parked at its host-call safepoint, and must neither pin
    // the arena nor come back into a chain whose code was retired.
    std::thread parked([&] {
        JitCodeArena::ArenaReadLease lease{
            JitCodeArena::ArenaReadLease::StableExecution{}};
        JitCodeArena::HostCallSafepoint safepoint{&lease};
        safepoint.park();
        parked_ready.store(true, std::memory_order_release);
        while (!parked_resume.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        run.parked_redispatched =
            safepoint.unpark() != JitCodeArena::HostCallSafepoint::Resume::Continue;
    });
    while (!parked_ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    const auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> workers;
    for (int w = 0; w < kWorkers; ++w) {
        workers.emplace_back([&, w] {
            worker_main(run, w, stop, run.stats[static_cast<usize>(w)]);
        });
    }
    // Wall cap so a stalled model still reports instead of hanging the gate.
    std::thread watchdog([&] {
        const auto deadline = start + std::chrono::seconds(60);
        while (!stop.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        stop.store(true, std::memory_order_release);
    });
    for (auto& t : workers) t.join();
    stop.store(true, std::memory_order_release);
    watchdog.join();
    run.seconds = std::chrono::duration<double>(
                      std::chrono::steady_clock::now() - start).count();
    parked_resume.store(true, std::memory_order_release);
    parked.join();

    run.refusals = JitCodeArena::arena_gate_refusals() - refusals_before;
    run.retirements = JitCodeArena::arena_reclaim_count() - retirements_before;
    run.abandons = JitCodeArena::arena_gate_abandons() - abandons_before;

    WorkerStats total;
    for (const WorkerStats& s : run.stats) {
        total.compiled += s.compiled;
        total.deferred += s.deferred;
        total.failed += s.failed;
        total.max_streak = std::max(total.max_streak, s.max_streak);
        total.executed += s.executed;
        total.stale += s.stale;
        total.wrong += s.wrong;
        total.lease_refused += s.lease_refused;
        total.hot_recompiles += s.hot_recompiles;
    }
    const u64 refused_total = run.refusals + total.lease_refused;
    std::printf(
        "[churn] budget=%zuMB block=%zuB workers=%d per_worker=%zu "
        "wall=%.2fs\n"
        "[churn] compiled=%llu deferred=%llu max_streak=%llu failed=%llu "
        "executed=%llu stale=%llu wrong=%llu hot_recompiles=%llu\n"
        "[churn] refused=%llu (gate=%llu chain_lease=%llu) retirements=%llu "
        "abandons=%llu parked_redispatched=%s\n"
        "[churn] whole-process interpretation window: %s\n",
        budget / (1024 * 1024), block_bytes, kWorkers, run.per_worker,
        run.seconds,
        static_cast<unsigned long long>(total.compiled),
        static_cast<unsigned long long>(total.deferred),
        static_cast<unsigned long long>(total.max_streak),
        static_cast<unsigned long long>(total.failed),
        static_cast<unsigned long long>(total.executed),
        static_cast<unsigned long long>(total.stale),
        static_cast<unsigned long long>(total.wrong),
        static_cast<unsigned long long>(total.hot_recompiles),
        static_cast<unsigned long long>(refused_total),
        static_cast<unsigned long long>(run.refusals),
        static_cast<unsigned long long>(total.lease_refused),
        static_cast<unsigned long long>(run.retirements),
        static_cast<unsigned long long>(run.abandons),
        run.parked_redispatched ? "yes" : "no",
        refused_total != 0 ? "YES (native entry was refused)" : "no");

    EXPECT_EQ(total.wrong, 0u) << "recycled arena bytes were executed";
    EXPECT_EQ(total.failed, 0u) << "a compilable block was refused permanently";
    EXPECT_GT(run.retirements, 0u) << "the arena never wrapped";
    EXPECT_GT(total.compiled, 0u);
    // The chunk model's acceptance criterion: wrapping the arena must never
    // refuse native entry. A refusal means the drain gate closed, i.e. the
    // process fell back to whole-process interpretation for a window.
    EXPECT_EQ(refused_total, 0u)
        << "the arena gate closed: retired chunks stayed pending";
    EXPECT_EQ(JitCodeArena::arena_pending_free_count(), 0u)
        << "retired chunks never freed";
    EXPECT_TRUE(run.parked_redispatched)
        << "a thread parked across retirements resumed into stale pointers";
}
