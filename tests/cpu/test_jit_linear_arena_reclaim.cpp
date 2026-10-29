// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// JIT code arena chunk retirement, rollover and execution leases. Split from
// test_jit_linear.cpp; shared fixtures and matchers live in
// jit_linear_fixture.hpp.

#include "jit_linear_fixture.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test;

namespace {

// Retiring one chunk must NOT invalidate code in the other chunks, and must
// need no drain: other threads keep their leases and keep executing. That is
// what lets a small arena budget survive without discarding the whole process
// compile history — or stopping the world — on every retirement.
TEST_F(JitLinearTest, ChunkRetirementKeepsCodeInOtherChunksWithoutDrain) {
    std::vector<u8> code = {0xB8, 0x78, 0x56, 0x34, 0x12};
    ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
    BasicBlock block_a = build_block(code);
    CompiledBlock* compiled_a = m_jit->compile(block_a);
    ASSERT_NE(compiled_a, nullptr);
    const CompiledBlock a = *compiled_a;
    // Force the next block into a different chunk.
    m_jit->release_arena_chunk(/*seal=*/true);
    BasicBlock block_b = build_block(code);
    CompiledBlock* compiled_b = m_jit->compile(block_b);
    ASSERT_NE(compiled_b, nullptr);
    const CompiledBlock b = *compiled_b;
    ASSERT_NE(JitCodeArena::arena_chunk_index(a.code),
              JitCodeArena::arena_chunk_index(b.code));
    ASSERT_TRUE(JitCodeArena::arena_code_live(a.code, a.arena_generation));
    ASSERT_TRUE(JitCodeArena::arena_code_live(b.code, b.arena_generation));

    // Another thread sits inside a dispatch chain for the whole retirement.
    std::atomic<bool> ready{false};
    std::atomic<bool> done{false};
    std::thread holder([&] {
        JitCodeArena::ArenaReadLease lease{
            JitCodeArena::ArenaReadLease::StableExecution{}};
        ASSERT_TRUE(static_cast<bool>(lease));
        ready.store(true, std::memory_order_release);
        while (!done.load(std::memory_order_acquire)) std::this_thread::yield();
    });
    while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();

    const u64 refusals_before = JitCodeArena::arena_gate_refusals();
    // Cold victims first: A's chunk is sealed and unowned; B's is this
    // compiler's current chunk and must never be chosen while alternatives
    // exist. Chunks left over from earlier tests may go first.
    usize rounds = 0;
    while (JitCodeArena::arena_code_live(a.code, a.arena_generation) &&
           rounds < JitCodeArena::arena_chunk_count()) {
        ASSERT_EQ(JitCodeArena::retire_arena_chunks(1, /*force=*/false), 1u);
        ++rounds;
    }
    EXPECT_FALSE(JitCodeArena::arena_code_live(a.code, a.arena_generation));
    EXPECT_TRUE(JitCodeArena::arena_code_live(b.code, b.arena_generation))
        << "the allocating chunk was retired while others were available";
    EXPECT_FALSE(JitCodeArena::arena_reclaim_pending()) << "gate closed";
    EXPECT_EQ(JitCodeArena::arena_gate_refusals(), refusals_before);

    CpuState cpu;
    init_cpu(cpu);
    CompiledBlock a_copy = a;
    EXPECT_EQ(m_jit->execute(a_copy, cpu), JitExecResult::StaleCode);
    CompiledBlock b_copy = b;
    EXPECT_EQ(m_jit->execute(b_copy, cpu), JitExecResult::Continue);
    EXPECT_EQ(cpu.gpr.get32(EAX), 0x12345678u);
    done.store(true, std::memory_order_release);
    holder.join();
}

TEST_F(JitLinearTest, ArenaRetirementInvalidatesPublishedNativePointer) {
    std::vector<u8> code = {0xB8, 0x78, 0x56, 0x34, 0x12};
    ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
    BasicBlock block = build_block(code);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    const CompiledBlock stale = *compiled;
    const uintptr_t stale_pc = reinterpret_cast<uintptr_t>(stale.code);
    ASSERT_TRUE(jit_code_range_contains(stale_pc));

    publish_compiled(block, *compiled);
    m_cache->insert(block);

    JitCodeArena::request_arena_reclaim();

    CpuState gated_cpu;
    init_cpu(gated_cpu);
    CompiledBlock gated_copy = stale;
    EXPECT_EQ(m_jit->execute(gated_copy, gated_cpu),
              JitExecResult::StaleCode);

    // Advance the ring until the slot this block was compiled into is
    // recycled, which is what raises the retirement horizon past it.
    const usize max_advances =
        JitCodeArena::arena_budget() / JitCodeArena::arena_capacity() + 4;
    usize advances = 0;
    ASSERT_TRUE(JitCodeArena::reclaim_arena_if_requested());
    while (JitCodeArena::arena_min_generation() <= stale.arena_generation &&
           advances < max_advances) {
        JitCodeArena::request_arena_reclaim();
        ASSERT_TRUE(JitCodeArena::reclaim_arena_if_requested());
        ++advances;
    }
    ASSERT_GT(JitCodeArena::arena_min_generation(), stale.arena_generation)
        << "oldest slot never retired within the arena budget";
    EXPECT_LE(JitCodeArena::arena_bytes_mapped(), JitCodeArena::arena_budget());
    // The reservation itself is stable: recycling a slot must not churn the
    // signal handler's code-range registration, whose snapshot pool is a
    // finite per-process resource.
    EXPECT_TRUE(jit_code_range_contains(stale_pc));

    CpuState cpu;
    init_cpu(cpu);
    CompiledBlock published_copy = stale;
    EXPECT_EQ(m_jit->execute(published_copy, cpu), JitExecResult::StaleCode);
    // execute() purges this thread's caches before it refuses, so the explicit
    // dispatcher-side sync has nothing left to do.
    EXPECT_FALSE(m_jit->synchronize_arena_generation());
    // A retirement drops the native code and keeps the decode — but this
    // block is fully native, so publish_compiled already released its decoded
    // array (the generated code no longer refers to it) while leaving
    // instruction_count set. Keeping that entry would leave it recompilable
    // with no instructions behind it, and compile() and the interpreter block
    // path both walk [0, instruction_count) through `instructions`: a host
    // null dereference on the next execution. It is evicted instead, so the
    // next execution re-decodes. See BlockCacheTest.
    // DropCompiledEvictsEntriesThatLostTheirDecode for the entry that does
    // still hold its decode and therefore survives.
    EXPECT_EQ(m_cache->lookup(block.start_addr), nullptr);

    BasicBlock replacement_block = build_block(code);
    CompiledBlock* replacement = m_jit->compile(replacement_block);
    ASSERT_NE(replacement, nullptr);
    EXPECT_TRUE(jit_code_range_contains(
        reinterpret_cast<uintptr_t>(replacement->code)));
    EXPECT_EQ(m_jit->execute(*replacement, cpu), JitExecResult::Continue);
    EXPECT_EQ(cpu.gpr.get32(EAX), 0x12345678u);
}

// A compile that fails only because the arena is mid-rollover must be
// distinguishable from one that fails because the block is uncompilable. The
// dispatcher records the latter permanently, so conflating them demotes a
// perfectly compilable block to the interpreter for the life of the process
// every time the arena rolls over.
// A zero-instruction BasicBlock cannot represent guest code; compiling one
// used to emit a bare prologue whose execution ran off the end of the emitted
// bytes into unwritten arena (host SIGILL). StarCraft II reached this through
// a guest null-pointer call whose rip=0 "block" arrived as a zeroed husk. The
// compiler must refuse it as a permanent (not deferred) failure so dispatch
// falls to the interpreter, which raises the guest-visible access violation.
TEST_F(JitLinearTest, EmptyBlockIsRefusedNotCompiled) {
    BasicBlock empty{};
    ASSERT_EQ(empty.instruction_count, 0u);
    EXPECT_EQ(m_jit->compile(empty), nullptr);
    EXPECT_FALSE(m_jit->compile_deferred())
        << "an empty block must be refused permanently, not retried";
}

TEST_F(JitLinearTest, ArenaRolloverDefersCompilationInsteadOfFailingIt) {
    std::vector<u8> code = {0xB8, 0x78, 0x56, 0x34, 0x12};
    ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
    BasicBlock block = build_block(code);

    JitCodeArena::request_arena_reclaim();
    EXPECT_EQ(m_jit->compile(block), nullptr);
    EXPECT_TRUE(m_jit->compile_deferred());

    ASSERT_TRUE(JitCodeArena::reclaim_arena_if_requested());
    (void)m_jit->synchronize_arena_generation();
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    EXPECT_FALSE(m_jit->compile_deferred());
}

// The signal handler's JIT code-range table publishes an immutable snapshot on
// every change and never reuses one, so its pool is a hard per-process budget.
// An arena that registered and unregistered a range per rollover exhausted it
// and then failed every registration, which disables native compilation
// process-wide — the guest keeps running, silently interpreted, forever.
TEST_F(JitLinearTest, RepeatedArenaRolloverKeepsCompilingNatively) {
    std::vector<u8> code = {0xB8, 0x78, 0x56, 0x34, 0x12};
    ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));

    const usize slots =
        JitCodeArena::arena_budget() / JitCodeArena::arena_capacity();
    const usize rollovers = slots * 3 + 8;
    for (usize i = 0; i < rollovers; ++i) {
        JitCodeArena::request_arena_reclaim();
        ASSERT_TRUE(JitCodeArena::reclaim_arena_if_requested()) << "i=" << i;
    }
    EXPECT_LE(JitCodeArena::arena_bytes_mapped(), JitCodeArena::arena_budget());

    // Same order the dispatcher uses: adopt the new horizon, then compile.
    (void)m_jit->synchronize_arena_generation();
    BasicBlock block = build_block(code);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    ASSERT_NE(compiled->code, nullptr)
        << "native compilation was disabled by arena rollover";
    EXPECT_TRUE(
        jit_code_range_contains(reinterpret_cast<uintptr_t>(compiled->code)));

    CpuState cpu;
    init_cpu(cpu);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    EXPECT_EQ(cpu.gpr.get32(EAX), 0x12345678u);
}

TEST_F(JitLinearTest, StableExecutionLeaseGuardsArenaReclaim) {
    std::vector<u8> code = {0xB8, 0x78, 0x56, 0x34, 0x12};
    ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
    BasicBlock block = build_block(code);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    std::atomic<bool> lease_ready{false};
    std::atomic<bool> release_lease{false};
    std::thread executor([&] {
        JitCodeArena::ArenaReadLease lease{
            JitCodeArena::ArenaReadLease::StableExecution{}};
        lease_ready.store(true, std::memory_order_release);
        while (!release_lease.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });
    while (!lease_ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    JitCodeArena::request_arena_reclaim();
    std::atomic<bool> reclaimed{false};
    EXPECT_TRUE(JitCodeArena::arena_reclaim_pending());
    std::atomic<bool> returned{false};
    std::thread reclaimer([&] {
        reclaimed.store(JitCodeArena::reclaim_arena_if_requested(),
                        std::memory_order_release);
        returned.store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(1);
    while (!returned.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    EXPECT_TRUE(returned.load(std::memory_order_acquire))
        << "dispatcher parked waiting for a lease needing guest progress";
    EXPECT_FALSE(reclaimed.load(std::memory_order_acquire));
    release_lease.store(true, std::memory_order_release);
    executor.join();
    reclaimer.join();
    if (!reclaimed.load(std::memory_order_acquire)) {
        EXPECT_TRUE(JitCodeArena::reclaim_arena_if_requested());
    }
    EXPECT_LE(JitCodeArena::arena_bytes_mapped(), JitCodeArena::arena_budget());
}


// The arena occupancy counter is sharded per thread; the reclaim drain must
// still be exact with many concurrent holders. Every held lease pins the
// arena, and the reclaim must complete promptly once the last one drops.
TEST_F(JitLinearTest, ConcurrentLeasesEachBlockArenaReclaim) {
    constexpr int kHolders = 8;
    std::atomic<int> ready{0};
    std::array<std::atomic<bool>, kHolders> release_lease{};
    for (auto& flag : release_lease) {
        flag.store(false, std::memory_order_release);
    }
    std::vector<std::thread> holders;
    for (int i = 0; i < kHolders; ++i) {
        holders.emplace_back([&, i] {
            JitCodeArena::ArenaReadLease lease{
                JitCodeArena::ArenaReadLease::StableExecution{}};
            ASSERT_TRUE(static_cast<bool>(lease));
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (!release_lease[static_cast<usize>(i)].load(
                       std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        });
    }
    while (ready.load(std::memory_order_acquire) != kHolders) {
        std::this_thread::yield();
    }

    JitCodeArena::request_arena_reclaim();
    // Dropping all but one lease must not let the reclaim through.
    for (int i = 0; i + 1 < kHolders; ++i) {
        release_lease[static_cast<usize>(i)].store(
            true, std::memory_order_release);
        holders[static_cast<usize>(i)].join();
    }
    EXPECT_FALSE(JitCodeArena::reclaim_arena_if_requested())
        << "reclaim proceeded while a lease was still held";

    release_lease[kHolders - 1].store(true, std::memory_order_release);
    holders.back().join();
    EXPECT_TRUE(JitCodeArena::reclaim_arena_if_requested());
    EXPECT_LE(JitCodeArena::arena_bytes_mapped(), JitCodeArena::arena_budget());
}

TEST_F(JitLinearTest, WorkerChainCanYieldToPendingArenaReclaim) {
    std::atomic<bool> ready{false};
    std::atomic<bool> yielded{false};
    std::thread worker([&] {
        JitCodeArena::ArenaReadLease lease;
        ASSERT_TRUE(static_cast<bool>(lease));
        ready.store(true, std::memory_order_release);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(1);
        // A worker without an event pump must see the same pending gate as
        // a GUI dispatcher. It may release only at a native-return boundary.
        while (!JitCodeArena::arena_reclaim_pending() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        yielded.store(JitCodeArena::arena_reclaim_pending(),
                      std::memory_order_release);
    });
    while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
    JitCodeArena::request_arena_reclaim();
    worker.join();
    EXPECT_TRUE(yielded.load(std::memory_order_acquire));
    EXPECT_TRUE(JitCodeArena::reclaim_arena_if_requested());
    EXPECT_FALSE(JitCodeArena::arena_reclaim_pending());
}

// A lease survives a guest callback or wait, so a guest thread parked in a
// wait pins the arena for as long as it stays parked. In a many-threaded guest
// there may be no instant at which every lease is free. The gate must not turn
// that into a permanent loss of native execution: the closed window is bounded,
// and an attempt that cannot drain gives the gate back and retries later.
// StarCraft II hit exactly this — the gate closed once and never reopened, so
// the whole process ran interpreted for the rest of its life.
TEST_F(JitLinearTest, UndrainableLeaseDoesNotCloseTheGateForever) {
    std::atomic<bool> release_lease{false};
    std::atomic<bool> ready{false};
    // Stands in for a guest thread parked in a wait while holding its lease.
    std::thread parked([&] {
        JitCodeArena::ArenaReadLease lease{
            JitCodeArena::ArenaReadLease::StableExecution{}};
        ASSERT_TRUE(static_cast<bool>(lease));
        ready.store(true, std::memory_order_release);
        while (!release_lease.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });
    while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();

    JitCodeArena::request_arena_reclaim();
    ASSERT_TRUE(JitCodeArena::arena_reclaim_pending());

    // The reclaim can never complete while that lease is held...
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (JitCodeArena::arena_reclaim_pending() &&
           std::chrono::steady_clock::now() < deadline) {
        EXPECT_FALSE(JitCodeArena::reclaim_arena_if_requested());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // ...so the gate must have been given back rather than left shut, and a
    // native entry must be admitted again even though nothing was reclaimed.
    EXPECT_FALSE(JitCodeArena::arena_reclaim_pending())
        << "gate stayed closed for a lease that never drains";
    {
        JitCodeArena::ArenaReadLease admitted;
        EXPECT_TRUE(static_cast<bool>(admitted))
            << "native entry still refused after the gate was given back";
    }

    release_lease.store(true, std::memory_order_release);
    parked.join();
}

}  // namespace
