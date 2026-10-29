// The code arena's resident soft budget: once live chunks would pass it, the
// coldest are retired before a fresh chunk is armed, and the sweep releases
// their pages from the process footprint.
//
// The arena is created once per process at the first JitCompiler, so it is
// shaped from a static initializer: 64 x 1 MB chunks, 16 MB soft budget (so
// a round's hysteresis margin, max(soft/8, one chunk), is two chunks). An
// already-set variable wins, which is how the DISABLED benchmark is sized.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <pthread.h>

#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/memory/guest_memory.hpp"
#include "core/memory/host_memory_floor.hpp"
#include "jit_dispatch_execute.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

namespace {

const bool g_env_configured = [] {
    setenv("VITREO_JIT_ARENA_MB", "64", 0);
    setenv("VITREO_JIT_ARENA_CHUNK_MB", "1", 0);
    setenv("VITREO_JIT_ARENA_SOFT_MB", "16", 0);
    // The budgets and compile bounds below were calibrated with a 1/64
    // far-branch island (64 KiB per 1 MB chunk); these 32-bit blocks have no
    // lazy slow paths to materialize into a larger one.
    setenv("VITREO_JIT_ARENA_ISLAND_DIV", "64", 0);
    return true;
}();

constexpr usize kMB = 1024 * 1024;
constexpr usize kSoft = 16 * kMB;
constexpr usize kChunk = 1 * kMB;
constexpr u8 EAX = static_cast<u8>(RegIndex::RAX);
constexpr u8 ESP = static_cast<u8>(RegIndex::RSP);
constexpr GuestAddr kCodeBase = 0x10000;
constexpr usize kStride = 8;

// Pressure must not shrink the budget mid-test.
struct PinnedNormalPressure {
    memory::HostMemoryReadings readings{};
    PinnedNormalPressure() {
        readings.pressure = memory::HostMemoryPressure::normal;
        memory::set_host_memory_readings_override(&readings);
    }
    ~PinnedNormalPressure() { memory::set_host_memory_readings_override(nullptr); }
};

u32 block_value(usize index) {
    return static_cast<u32>(index * 0x85EBCA6Bu) ^ 0x5A5A0000u;
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

u64 phys_footprint() {
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) {
        return 0;
    }
    return info.phys_footprint;
}

void settle_and_sweep() {
    // The sweep is rate limited to once per millisecond.
    for (int i = 0; i < 20 && JitCodeArena::arena_pending_free_count() != 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        (void)JitCodeArena::sweep_retired_chunks();
    }
}

// Block i at kCodeBase + i * kStride is MOV EAX, block_value(i).
bool fill_mov_blocks(GuestMemory& memory, usize count) {
    if (!memory.map_region(kCodeBase, count * kStride + 0x1000,
                           MemoryProtection::ReadWrite)) {
        return false;
    }
    for (usize i = 0; i < count; ++i) {
        const u32 v = block_value(i);
        const u8 code[5] = {0xB8, static_cast<u8>(v), static_cast<u8>(v >> 8),
                            static_cast<u8>(v >> 16), static_cast<u8>(v >> 24)};
        if (!memory.write_bytes(kCodeBase + i * kStride, code, sizeof(code))) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST(JitArenaSoftBudget, CompilingPastTheBudgetRetiresAndNeverRunsRetiredCode) {
    ASSERT_TRUE(g_env_configured);
    PinnedNormalPressure pinned;
    GuestMemory memory;
    constexpr usize kBlocks = 600000;  // ~3x the soft budget at ~96 B/block
    ASSERT_TRUE(memory.map_region(kCodeBase, kBlocks * kStride + 0x1000,
                                  MemoryProtection::ReadWrite));
    for (usize i = 0; i < kBlocks; ++i) {
        const u32 v = block_value(i);
        const u8 code[5] = {0xB8, static_cast<u8>(v), static_cast<u8>(v >> 8),
                            static_cast<u8>(v >> 16), static_cast<u8>(v >> 24)};
        ASSERT_TRUE(memory.write_bytes(kCodeBase + i * kStride, code, sizeof(code)));
    }

    BlockCache cache(4096);
    JitCompiler jit(memory);
    jit.set_block_cache(&cache);
    CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.set32(ESP, 0x10000);

    ASSERT_EQ(JitCodeArena::arena_soft_budget(), kSoft);
    ASSERT_EQ(JitCodeArena::arena_capacity(), kChunk);
    const u64 retired_before = JitCodeArena::arena_reclaim_count();

    struct Snapshot { CompiledBlock cb; GuestAddr addr; u32 value; };
    std::vector<Snapshot> early;
    usize max_live = 0;
    u64 wrong = 0;
    for (usize i = 0; i < kBlocks; ++i) {
        (void)JitCodeArena::reclaim_arena_if_requested();
        (void)jit.synchronize_arena_generation();
        JitCodeArena::ArenaReadLease chain{
            JitCodeArena::ArenaReadLease::StableExecution{}};
        ASSERT_TRUE(static_cast<bool>(chain));
        const GuestAddr addr = kCodeBase + i * kStride;
        BasicBlock* entry = cache.insert(decode_mov(addr, block_value(i)));
        CompiledBlock* compiled = jit.compile(*entry);
        if (compiled == nullptr) {
            ASSERT_TRUE(jit.compile_deferred()) << "block " << i;
            continue;
        }
        jit.publish_compiled_block(*entry, *compiled);
        CompiledBlock cb = compiled_from_cache(*entry);
        cpu.rip = addr;
        cpu.gpr.set32(EAX, 0);
        if (jit.execute(cb, cpu) != JitExecResult::StaleCode &&
            cpu.gpr.get32(EAX) != block_value(i)) {
            ++wrong;
        }
        if (early.size() < 256 && (i % 7) == 0) {
            early.push_back(Snapshot{cb, addr, block_value(i)});
        }
        if ((i % 256) == 0) {
            max_live = std::max(max_live, JitCodeArena::arena_live_bytes());
        }
    }
    max_live = std::max(max_live, JitCodeArena::arena_live_bytes());
    std::printf("[soft-budget] retired=%llu max_live=%zuMB live=%zuMB\n",
                static_cast<unsigned long long>(
                    JitCodeArena::arena_reclaim_count() - retired_before),
                max_live / kMB, JitCodeArena::arena_live_bytes() / kMB);

    EXPECT_EQ(wrong, 0u);
    EXPECT_GT(JitCodeArena::arena_reclaim_count(), retired_before)
        << "filling 3x the soft budget never retired a chunk";
    EXPECT_LE(max_live, kSoft + kChunk);

    // Pointers published before their chunk was retired (and, by now,
    // recycled and refilled) must be refused, never entered.
    usize stale = 0;
    {
        JitCodeArena::ArenaReadLease chain{
            JitCodeArena::ArenaReadLease::StableExecution{}};
        ASSERT_TRUE(static_cast<bool>(chain));
        for (Snapshot& s : early) {
            cpu.rip = s.addr;
            cpu.gpr.set32(EAX, 0xDEADBEEFu);
            const JitExecResult result = jit.execute(s.cb, cpu);
            if (result == JitExecResult::StaleCode) {
                ++stale;
                EXPECT_EQ(cpu.gpr.get32(EAX), 0xDEADBEEFu)
                    << "retired code ran for block at 0x" << std::hex << s.addr;
            } else {
                EXPECT_EQ(cpu.gpr.get32(EAX), s.value);
            }
        }
    }
    EXPECT_GT(stale, 0u) << "no early block was retired";
    settle_and_sweep();
    EXPECT_EQ(JitCodeArena::arena_pending_free_count(), 0u);
}

TEST(JitArenaSoftBudget, RetiredChunkPagesLeaveTheFootprint) {
    PinnedNormalPressure pinned;
    JitCodeArena::ensure_initialized();
    ASSERT_EQ(JitCodeArena::arena_soft_budget(), kSoft);
    // Start empty: a whole-chunk allocation cannot resume a partial chunk.
    (void)JitCodeArena::retire_arena_chunks(JitCodeArena::arena_chunk_count(),
                                            /*force=*/true);
    settle_and_sweep();
    ASSERT_EQ(JitCodeArena::arena_live_bytes(), 0u);
    const u64 baseline = phys_footprint();
    ASSERT_NE(baseline, 0u);
    u64 peak = baseline;

    // Dirty three soft budgets' worth of whole chunks, one per allocation.
    JitCodeArena::Cursor cursor;
    const usize fill = JitCodeArena::chunk_code_capacity();
    for (usize i = 0; i < 3 * (kSoft / kChunk); ++i) {
        {
            JitCodeArena::ArenaReadLease lease;
            ASSERT_TRUE(static_cast<bool>(lease));
            u64 generation = 0;
            void* mem = JitCodeArena::alloc(cursor, fill, generation);
            ASSERT_NE(mem, nullptr) << "allocation " << i;
            pthread_jit_write_protect_np(0);
            std::memset(mem, 0xA5, fill);
            pthread_jit_write_protect_np(1);
            sys_icache_invalidate(mem, fill);
        }
        settle_and_sweep();
        peak = std::max(peak, phys_footprint());
    }
    JitCodeArena::release_chunk(cursor);
    std::printf("[soft-budget] footprint growth peak=%lluMB (soft=%zuMB)\n",
                static_cast<unsigned long long>((peak - baseline) / kMB),
                kSoft / kMB);
    EXPECT_LE(JitCodeArena::arena_live_bytes(), kSoft + kChunk);
    EXPECT_EQ(JitCodeArena::arena_pending_free_count(), 0u);
    // 3x the budget was dirtied; only what it keeps live may stay charged.
    EXPECT_LE(peak - baseline, kSoft + 2 * kChunk);
}

// A soft-budget round retires down to soft - max(soft/8, one chunk), so the
// live footprint right after the round (the victims gone, the one fresh
// chunk armed) is at most that target plus a chunk, never the budget itself;
// and no block whose chunk a round retired is ever entered again.
TEST(JitArenaSoftBudget, ARoundRetiresDownToTheHysteresisTarget) {
    PinnedNormalPressure pinned;
    GuestMemory memory;
    constexpr usize kBlocks = 600000;
    ASSERT_TRUE(fill_mov_blocks(memory, kBlocks));
    BlockCache cache(4096);
    JitCompiler jit(memory);
    jit.set_block_cache(&cache);
    CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.set32(ESP, 0x10000);

    ASSERT_EQ(JitCodeArena::arena_soft_budget(), kSoft);
    ASSERT_EQ(JitCodeArena::arena_capacity(), kChunk);
    const usize margin = std::max(kSoft / 8, kChunk);
    ASSERT_GT(margin, kChunk) << "the shaping must make a round retire >1 chunk";
    const usize target = kSoft - margin;

    struct Snapshot { CompiledBlock cb; GuestAddr addr; u32 value; };
    std::vector<Snapshot> early;
    const u64 retired_before = JitCodeArena::arena_reclaim_count();
    u64 seq = JitCodeArena::arena_retire_seq();
    u64 rounds = 0;
    u64 wrong = 0;
    usize max_live = 0;
    usize max_live_after_round = 0;
    for (usize i = 0; i < kBlocks; ++i) {
        (void)JitCodeArena::reclaim_arena_if_requested();
        (void)jit.synchronize_arena_generation();
        JitCodeArena::ArenaReadLease chain{
            JitCodeArena::ArenaReadLease::StableExecution{}};
        ASSERT_TRUE(static_cast<bool>(chain));
        const GuestAddr addr = kCodeBase + i * kStride;
        BasicBlock* entry = cache.insert(decode_mov(addr, block_value(i)));
        CompiledBlock* compiled = jit.compile(*entry);
        const u64 now_seq = JitCodeArena::arena_retire_seq();
        if (now_seq != seq) {
            rounds += now_seq - seq;
            seq = now_seq;
            const usize live = JitCodeArena::arena_live_bytes();
            max_live_after_round = std::max(max_live_after_round, live);
            EXPECT_LE(live, target + kChunk) << "round at block " << i;
        }
        max_live = std::max(max_live, JitCodeArena::arena_live_bytes());
        if (compiled == nullptr) {
            ASSERT_TRUE(jit.compile_deferred()) << "block " << i;
            continue;
        }
        jit.publish_compiled_block(*entry, *compiled);
        CompiledBlock cb = compiled_from_cache(*entry);
        cpu.rip = addr;
        cpu.gpr.set32(EAX, 0);
        if (test::execute_like_dispatcher(jit, cb, cpu) !=
                JitExecResult::StaleCode &&
            cpu.gpr.get32(EAX) != block_value(i)) {
            ++wrong;
        }
        if (early.size() < 256 && (i % 7) == 0) {
            early.push_back(Snapshot{cb, addr, block_value(i)});
        }
    }
    const u64 retired = JitCodeArena::arena_reclaim_count() - retired_before;
    std::printf("[soft-budget] hysteresis rounds=%llu retired=%llu "
                "max_live=%zuMB max_live_after_round=%zuMB target=%zuMB\n",
                static_cast<unsigned long long>(rounds),
                static_cast<unsigned long long>(retired), max_live / kMB,
                max_live_after_round / kMB, target / kMB);
    EXPECT_EQ(wrong, 0u);
    ASSERT_GT(rounds, 0u) << "filling 3x the soft budget never retired";
    EXPECT_LE(max_live, kSoft);
    // Each round frees the whole margin, not just room for one chunk.
    EXPECT_GE(retired, rounds * (margin / kChunk));

    usize stale = 0;
    {
        JitCodeArena::ArenaReadLease chain{
            JitCodeArena::ArenaReadLease::StableExecution{}};
        ASSERT_TRUE(static_cast<bool>(chain));
        for (Snapshot& s : early) {
            cpu.rip = s.addr;
            cpu.gpr.set32(EAX, 0xDEADBEEFu);
            if (jit.execute(s.cb, cpu) == JitExecResult::StaleCode) {
                ++stale;
                EXPECT_EQ(cpu.gpr.get32(EAX), 0xDEADBEEFu)
                    << "retired code ran for block at 0x" << std::hex << s.addr;
            } else {
                EXPECT_EQ(cpu.gpr.get32(EAX), s.value);
            }
        }
    }
    EXPECT_GT(stale, 0u) << "no early block was retired";
    settle_and_sweep();
    EXPECT_EQ(JitCodeArena::arena_pending_free_count(), 0u);
}

// Microbenchmark for the cost of retirement rounds themselves. Several
// dispatcher-shaped threads, each with a large block cache, compile cold code
// continuously past the soft budget while re-executing a slowly rotating hot
// set; every round makes every thread walk its cache at its next safe point.
// Same binary, both policies:
//   VITREO_JIT_ARENA_MB=512 VITREO_JIT_ARENA_SOFT_MB=64 \
//   [VITREO_JIT_ARENA_RETIRE_ONE=1] vitreo_jit_arena_soft_budget_tests \
//   --gtest_also_run_disabled_tests \
//   --gtest_filter=JitArenaSoftBudget.DISABLED_RetirementRoundCost
TEST(JitArenaSoftBudget, DISABLED_RetirementRoundCost) {
    PinnedNormalPressure pinned;
    JitCodeArena::ensure_initialized();
    const usize soft = JitCodeArena::arena_soft_budget();
    constexpr usize kThreads = 4;
    constexpr usize kHotRange = 8192;
    constexpr usize kHotWindow = 1024;
    constexpr usize kSlideEvery = 16;
    constexpr usize kHotPerIter = 16;
    constexpr usize kCacheBlocks = 65536;
    constexpr usize kBytesPerBlock = 96;
    constexpr usize kBudgetsOfCode = 3;
    const usize cold = kBudgetsOfCode * soft / kBytesPerBlock / kThreads;
    const usize per_thread = kHotRange + cold;
    GuestMemory memory;
    ASSERT_TRUE(fill_mov_blocks(memory, kThreads * per_thread));

    std::atomic<u64> executions{0}, recompiles{0}, deferred{0}, wrong{0},
        stale_twice{0};
    std::atomic<usize> ready{0};
    std::atomic<bool> go{false};
    const u64 seq_before = JitCodeArena::arena_retire_seq();
    const u64 retired_before = JitCodeArena::arena_reclaim_count();

    auto worker = [&](usize t) {
        BlockCache cache(kCacheBlocks);
        JitCompiler jit(memory);
        jit.set_block_cache(&cache);
        CpuState cpu;
        cpu.init_protected_mode();
        cpu.gpr.set32(ESP, 0x10000);
        std::vector<u8> hot_compiled(kHotRange, 0);
        u64 local_exec = 0, local_recompiles = 0, local_deferred = 0,
            local_wrong = 0, local_stale_twice = 0;
        const usize base = t * per_thread;

        const auto run_block = [&](usize idx) {
            const usize global = base + idx;
            const GuestAddr addr = kCodeBase + global * kStride;
            const u32 value = block_value(global);
            BasicBlock* entry = cache.lookup(addr);
            if (entry == nullptr) entry = cache.insert(decode_mov(addr, value));
            if (entry->compiled_code == nullptr) {
                CompiledBlock* compiled = jit.compile(*entry);
                if (compiled == nullptr) {
                    ++local_deferred;
                    return;
                }
                if (idx < kHotRange) {
                    if (hot_compiled[idx] != 0) ++local_recompiles;
                    hot_compiled[idx] = 1;
                }
                jit.publish_compiled_block(*entry, *compiled);
            }
            CompiledBlock cb = compiled_from_cache(*entry);
            cpu.rip = addr;
            cpu.gpr.set32(EAX, 0);
            // test::execute_like_dispatcher's retry, except that with several
            // threads another one may retire this block's own chunk between
            // the safe point and entry: that is a legitimate StaleCode here.
            JitExecResult result = jit.execute(cb, cpu);
            if (result == JitExecResult::StaleCode) result = jit.execute(cb, cpu);
            if (result == JitExecResult::StaleCode) {
                ++local_stale_twice;
                return;
            }
            ++local_exec;
            if (cpu.gpr.get32(EAX) != value) ++local_wrong;
        };

        ready.fetch_add(1);
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        for (usize i = 0; i < cold; ++i) {
            (void)JitCodeArena::reclaim_arena_if_requested();
            (void)jit.synchronize_arena_generation();
            JitCodeArena::ArenaReadLease chain{
                JitCodeArena::ArenaReadLease::StableExecution{}};
            if (!chain) continue;
            run_block(kHotRange + i);
            const usize window = (i / kSlideEvery) % kHotRange;
            for (usize k = 0; k < kHotPerIter; ++k) {
                run_block((window + (i * kHotPerIter + k) % kHotWindow) %
                          kHotRange);
            }
        }
        executions += local_exec;
        recompiles += local_recompiles;
        deferred += local_deferred;
        wrong += local_wrong;
        stale_twice += local_stale_twice;
    };

    std::vector<std::thread> threads;
    for (usize t = 0; t < kThreads; ++t) threads.emplace_back(worker, t);
    while (ready.load() != kThreads) std::this_thread::yield();
    const auto start = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (std::thread& thread : threads) thread.join();
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - start)
                          .count();

    const u64 rounds = JitCodeArena::arena_retire_seq() - seq_before;
    const u64 retired = JitCodeArena::arena_reclaim_count() - retired_before;
    const u64 execs = executions.load();
    std::printf("[retire-bench] policy=%s soft=%zuMB threads=%zu ms=%.1f "
                "execs=%llu ms_per_1M_execs=%.3f rounds=%llu retired=%llu "
                "recompiles=%llu deferred=%llu stale_twice=%llu\n",
                std::getenv("VITREO_JIT_ARENA_RETIRE_ONE") ? "one" : "batch",
                soft / kMB, kThreads, ms, static_cast<unsigned long long>(execs),
                execs != 0 ? ms * 1e6 / static_cast<double>(execs) : 0.0,
                static_cast<unsigned long long>(rounds),
                static_cast<unsigned long long>(retired),
                static_cast<unsigned long long>(recompiles.load()),
                static_cast<unsigned long long>(deferred.load()),
                static_cast<unsigned long long>(stale_twice.load()));
    EXPECT_EQ(wrong.load(), 0u);
    EXPECT_GT(rounds, 0u);
}

namespace {

// Start a test from an empty arena at the configured budget, and leave the
// next test there too: a budget raised by thrash is never lowered in
// production, but the tests share one process.
struct EmptyArenaAtConfiguredBudget {
    EmptyArenaAtConfiguredBudget() { reset(); }
    ~EmptyArenaAtConfiguredBudget() { reset(); }
    static void reset() {
        JitCodeArena::ensure_initialized();
        (void)JitCodeArena::retire_arena_chunks(
            JitCodeArena::arena_chunk_count(), /*force=*/true);
        settle_and_sweep();
        JitCodeArena::reset_soft_budget_learning();
    }
};

void write_mov(GuestMemory& memory, GuestAddr addr, u32 v) {
    const u8 code[5] = {0xB8, static_cast<u8>(v), static_cast<u8>(v >> 8),
                        static_cast<u8>(v >> 16), static_cast<u8>(v >> 24)};
    ASSERT_TRUE(memory.write_bytes(addr, code, sizeof(code)));
}

// One dispatcher step: safe point, lease, cached lookup, compile when the
// cache holds no live code for the block, execute. Returns whether the step
// compiled; `wrong` counts executions that did not load `value`.
bool run_block(JitCompiler& jit, BlockCache& cache, CpuState& cpu,
               GuestAddr addr, u32 value, u64& wrong) {
    (void)JitCodeArena::reclaim_arena_if_requested();
    (void)jit.synchronize_arena_generation();
    JitCodeArena::ArenaReadLease chain{
        JitCodeArena::ArenaReadLease::StableExecution{}};
    EXPECT_TRUE(static_cast<bool>(chain));
    BasicBlock* entry = cache.lookup(addr);
    if (entry == nullptr) entry = cache.insert(decode_mov(addr, value));
    bool compiled_now = false;
    if (entry->compiled_code == nullptr) {
        CompiledBlock* compiled = jit.compile(*entry);
        if (compiled == nullptr) {
            EXPECT_TRUE(jit.compile_deferred()) << "block 0x" << std::hex << addr;
            return false;
        }
        jit.publish_compiled_block(*entry, *compiled);
        compiled_now = true;
    }
    CompiledBlock cb = compiled_from_cache(*entry);
    cpu.rip = addr;
    cpu.gpr.set32(EAX, 0);
    if (test::execute_like_dispatcher(jit, cb, cpu) != JitExecResult::StaleCode &&
        cpu.gpr.get32(EAX) != value) {
        ++wrong;
    }
    return compiled_now;
}

}  // namespace

// A process whose live translated working set is larger than the soft budget
// (Satisfactory's menu: ~890 MB live against 512 MB) re-runs every block of
// it every frame. Retiring any of it reclaims nothing -- the block is
// translated again at once -- so the budget must yield to it: total compiles
// stay about one per block plus the bounded cost of noticing, instead of
// growing by the working set every pass.
TEST(JitArenaSoftBudget, ALiveWorkingSetAboveTheBudgetIsNotRecompiledEveryPass) {
    PinnedNormalPressure pinned;
    EmptyArenaAtConfiguredBudget empty;
    ASSERT_EQ(JitCodeArena::arena_soft_budget(), kSoft);
    // ~1.25 soft budgets of code at ~96 B/block.
    constexpr usize kBlocks = 220000;
    constexpr usize kPasses = 6;
    GuestMemory memory;
    ASSERT_TRUE(fill_mov_blocks(memory, kBlocks));
    BlockCache cache(2 * kBlocks);  // never evicts: every compile is the arena's
    JitCompiler jit(memory);
    jit.set_block_cache(&cache);
    CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.set32(ESP, 0x10000);

    const u64 retired_before = JitCodeArena::arena_reclaim_count();
    std::vector<u64> compiles(kPasses, 0);
    u64 wrong = 0;
    usize max_live = 0;
    for (usize pass = 0; pass < kPasses; ++pass) {
        for (usize i = 0; i < kBlocks; ++i) {
            if (run_block(jit, cache, cpu, kCodeBase + i * kStride,
                          block_value(i), wrong)) {
                ++compiles[pass];
            }
        }
        max_live = std::max(max_live, JitCodeArena::arena_live_bytes());
        std::printf("[soft-budget] working-set pass %zu: compiles=%llu "
                    "live=%zuMB retired=%llu\n",
                    pass, static_cast<unsigned long long>(compiles[pass]),
                    JitCodeArena::arena_live_bytes() / kMB,
                    static_cast<unsigned long long>(
                        JitCodeArena::arena_reclaim_count() - retired_before));
    }
    u64 total = 0;
    for (const u64 c : compiles) total += c;
    std::printf("[soft-budget] working-set total compiles=%llu blocks=%zu "
                "max_live=%zuMB raises=%u soft_now=%zuMB retranslated=%lluKB\n",
                static_cast<unsigned long long>(total), kBlocks, max_live / kMB,
                JitCodeArena::arena_soft_budget_raises(),
                JitCodeArena::arena_current_soft_budget() / kMB,
                static_cast<unsigned long long>(
                    JitCodeArena::arena_retranslated_bytes() / 1024));
    EXPECT_GE(JitCodeArena::arena_soft_budget_raises(), 1u);
    EXPECT_GT(JitCodeArena::arena_current_soft_budget(), kSoft);
    EXPECT_EQ(wrong, 0u);
    EXPECT_GT(JitCodeArena::arena_reclaim_count(), retired_before)
        << "the working set never passed the budget; the test proves nothing";
    // Once the budget has yielded, a pass re-runs the whole working set
    // without compiling anything.
    EXPECT_EQ(compiles[kPasses - 1], 0u);
    EXPECT_EQ(compiles[kPasses - 2], 0u);
    // One compile per block, plus what noticing costs.
    EXPECT_LE(total, kBlocks + kBlocks / 2);
}

// The case the soft budget exists for (a Steam CEF renderer: ~690 MB of arena
// pages against ~80 MB of live translations). The guest keeps regenerating
// code -- new code into a ring of code regions, each region's old code dead
// once rewritten -- while a small hot set runs throughout. Live arena bytes
// must stay at the budget: regenerated code is not "re-translation", even at
// an address whose old translation a round retired, because its bytes differ.
namespace {
void run_regenerating_guest(bool memo) {
    PinnedNormalPressure pinned;
    EmptyArenaAtConfiguredBudget empty;
    JitCompileMemo::clear_compile_memo();
    if (memo) JitCompileMemo::enable_compile_memo();
    struct MemoOff {
        ~MemoOff() {
            JitCompileMemo::disable_compile_memo();
            JitCompileMemo::clear_compile_memo();
        }
    } memo_off;
    ASSERT_EQ(JitCodeArena::arena_soft_budget(), kSoft);
    constexpr usize kHot = 20000;             // ~2 MB, well under the budget
    constexpr usize kRegionBlocks = 40000;    // ~4 MB of new code per round
    constexpr usize kRegions = 8;             // ring of 2x the budget
    constexpr usize kRounds = 32;             // ~8x the budget of churn
    constexpr usize kTotal = kHot + kRegions * kRegionBlocks;
    GuestMemory memory;
    ASSERT_TRUE(fill_mov_blocks(memory, kTotal));
    BlockCache cache(2 * kTotal);
    JitCompiler jit(memory);
    jit.set_block_cache(&cache);
    CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.set32(ESP, 0x10000);

    const u64 retired_before = JitCodeArena::arena_reclaim_count();
    u64 wrong = 0;
    usize max_live = 0;
    for (usize round = 0; round < kRounds; ++round) {
        // Regenerate one region: new bytes at its addresses, and the stale
        // cached decodes dropped as the executable-write observer would.
        const usize region = round % kRegions;
        const usize first = kHot + region * kRegionBlocks;
        const auto value = [round](usize i) {
            return block_value(i) ^ static_cast<u32>(round * 0x01000193u);
        };
        for (usize i = first; i < first + kRegionBlocks; ++i) {
            write_mov(memory, kCodeBase + i * kStride, value(i));
        }
        (void)cache.invalidate_range(kCodeBase + first * kStride,
                                     kRegionBlocks * kStride);
        for (usize i = first; i < first + kRegionBlocks; ++i) {
            (void)run_block(jit, cache, cpu, kCodeBase + i * kStride, value(i),
                            wrong);
            if ((i % 2) == 0) {
                const usize h = (i / 2) % kHot;
                (void)run_block(jit, cache, cpu, kCodeBase + h * kStride,
                                block_value(h), wrong);
            }
            if ((i % 256) == 0) {
                max_live = std::max(max_live, JitCodeArena::arena_live_bytes());
            }
        }
    }
    max_live = std::max(max_live, JitCodeArena::arena_live_bytes());
    std::printf("[soft-budget] regenerate retired=%llu max_live=%zuMB "
                "raises=%u retranslated=%lluKB\n",
                static_cast<unsigned long long>(
                    JitCodeArena::arena_reclaim_count() - retired_before),
                max_live / kMB, JitCodeArena::arena_soft_budget_raises(),
                static_cast<unsigned long long>(
                    JitCodeArena::arena_retranslated_bytes() / 1024));
    // Regenerated code is never mistaken for a working set that does not fit.
    EXPECT_EQ(JitCodeArena::arena_soft_budget_raises(), 0u);
    EXPECT_EQ(JitCodeArena::arena_current_soft_budget(), kSoft);
    EXPECT_EQ(wrong, 0u);
    EXPECT_GT(JitCodeArena::arena_reclaim_count(), retired_before);
    EXPECT_LE(max_live, kSoft + kChunk);
    settle_and_sweep();
    EXPECT_EQ(JitCodeArena::arena_pending_free_count(), 0u);
}
}  // namespace

TEST(JitArenaSoftBudget, RegeneratedCodeStaysWithinTheBudget) {
    run_regenerating_guest(/*memo=*/false);
}

TEST(JitArenaSoftBudget, RegeneratedCodeStaysWithinTheBudgetThroughTheMemo) {
    run_regenerating_guest(/*memo=*/true);
}

// The same working set as production runs it: the process-wide compile memo
// on, and a per-thread block cache far smaller than the working set (the
// runtime's is 65536 blocks), so most retired translations are in no thread's
// cache when their chunk goes -- only the memo sees them retired. Counted in
// fresh emissions, since an evicted block that the memo still holds comes
// back by adoption, without emitting.
TEST(JitArenaSoftBudget, AWorkingSetAboveTheBudgetStopsRecompilingThroughTheMemo) {
    PinnedNormalPressure pinned;
    EmptyArenaAtConfiguredBudget empty;
    JitCompileMemo::clear_compile_memo();
    JitCompileMemo::enable_compile_memo();
    struct MemoOff {
        ~MemoOff() {
            JitCompileMemo::disable_compile_memo();
            JitCompileMemo::clear_compile_memo();
        }
    } memo_off;
    constexpr usize kBlocks = 220000;
    constexpr usize kPasses = 6;
    GuestMemory memory;
    ASSERT_TRUE(fill_mov_blocks(memory, kBlocks));
    BlockCache cache(4096);
    JitCompiler jit(memory);
    jit.set_block_cache(&cache);
    CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.set32(ESP, 0x10000);

    const u64 retired_before = JitCodeArena::arena_reclaim_count();
    std::vector<u64> emits(kPasses, 0);
    u64 wrong = 0;
    for (usize pass = 0; pass < kPasses; ++pass) {
        const u64 before = JitCompileMemo::compile_memo_stats().fresh_emits;
        for (usize i = 0; i < kBlocks; ++i) {
            (void)run_block(jit, cache, cpu, kCodeBase + i * kStride,
                            block_value(i), wrong);
        }
        emits[pass] = JitCompileMemo::compile_memo_stats().fresh_emits - before;
        std::printf("[soft-budget] memo working-set pass %zu: emits=%llu "
                    "live=%zuMB retired=%llu\n",
                    pass, static_cast<unsigned long long>(emits[pass]),
                    JitCodeArena::arena_live_bytes() / kMB,
                    static_cast<unsigned long long>(
                        JitCodeArena::arena_reclaim_count() - retired_before));
    }
    u64 total = 0;
    for (const u64 e : emits) total += e;
    std::printf("[soft-budget] memo working-set total emits=%llu raises=%u "
                "soft_now=%zuMB retranslated=%lluKB\n",
                static_cast<unsigned long long>(total),
                JitCodeArena::arena_soft_budget_raises(),
                JitCodeArena::arena_current_soft_budget() / kMB,
                static_cast<unsigned long long>(
                    JitCodeArena::arena_retranslated_bytes() / 1024));
    EXPECT_EQ(wrong, 0u);
    EXPECT_GT(JitCodeArena::arena_reclaim_count(), retired_before);
    EXPECT_GE(JitCodeArena::arena_soft_budget_raises(), 1u);
    EXPECT_EQ(emits[kPasses - 1], 0u);
    EXPECT_EQ(emits[kPasses - 2], 0u);
    EXPECT_LE(total, kBlocks + kBlocks / 2);
}
