// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// Host-call safepoints and multi-thread chunk lifetime in the JIT code arena.
// Split from test_jit_linear.cpp; shared fixtures and matchers live in
// jit_linear_fixture.hpp.

#include "jit_linear_fixture.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test;

namespace {

// A host call made from compiled code is a safepoint: the chain lease is
// parked for its duration, so a thread blocked in Sleep/WaitForSingleObject/
// EnterCriticalSection contributes no lease and the drain completes even with
// the thread still parked. On return the thread must notice that a slot was
// retired while it was out and re-dispatch through the block lookup instead of
// continuing into a compiled pointer it cached before the call.
TEST_F(JitLinearTest, HostCallSafepointLetsReclaimDrainAndRedispatches) {
    using Safepoint = JitCodeArena::HostCallSafepoint;
    // Live code in the only slot, so the reclaim is a real retirement that
    // moves the horizon, not a virgin-slot advance.
    std::vector<u8> code = {0xB8, 0x78, 0x56, 0x34, 0x12};
    ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
    BasicBlock block = build_block(code);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    const u64 compiled_generation = compiled->arena_generation;

    std::atomic<bool> parked_ready{false};
    std::atomic<bool> host_call_done{false};
    std::atomic<bool> lease_held_after{false};
    std::atomic<Safepoint::Resume> resume{Safepoint::Resume::Continue};
    // A guest thread in a dispatch chain that enters a blocking host call.
    std::thread parked([&] {
        JitCodeArena::ArenaReadLease chain_lease{
            JitCodeArena::ArenaReadLease::StableExecution{}};
        ASSERT_TRUE(static_cast<bool>(chain_lease));
        Safepoint safepoint{&chain_lease};
        safepoint.park();
        EXPECT_FALSE(static_cast<bool>(chain_lease));
        EXPECT_FALSE(JitCodeArena::ArenaReadLease::thread_holds());
        parked_ready.store(true, std::memory_order_release);
        while (!host_call_done.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        resume.store(safepoint.unpark(), std::memory_order_release);
        lease_held_after.store(static_cast<bool>(chain_lease),
                               std::memory_order_release);
    });
    while (!parked_ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    // Advance the ring until the slot holding the block is recycled. Virgin
    // slots need no drain; the retiring advance does, and it must complete
    // with the thread still parked in its host call.
    const usize max_advances =
        JitCodeArena::arena_budget() / JitCodeArena::arena_capacity() + 4;
    usize advances = 0;
    bool drained = true;
    while (drained &&
           JitCodeArena::arena_min_generation() <= compiled_generation &&
           advances < max_advances) {
        JitCodeArena::request_arena_reclaim();
        drained = JitCodeArena::reclaim_arena_if_requested();
        ++advances;
    }
    EXPECT_TRUE(drained)
        << "a thread parked in a host call still pinned the arena";
    EXPECT_FALSE(JitCodeArena::arena_reclaim_pending());
    EXPECT_GT(JitCodeArena::arena_min_generation(), compiled_generation)
        << "reclaim did not retire the slot holding the compiled block";

    host_call_done.store(true, std::memory_order_release);
    parked.join();
    EXPECT_EQ(resume.load(std::memory_order_acquire),
              Safepoint::Resume::Redispatch)
        << "thread returned into a chain whose code was retired";
    EXPECT_TRUE(lease_held_after.load(std::memory_order_acquire))
        << "lease not taken back after the host call";
}

// Unchanged generation across the host call: the chain continues into its
// compiled continuation exactly as before, with the lease held again.
TEST_F(JitLinearTest, HostCallSafepointUnchangedGenerationContinues) {
    using Safepoint = JitCodeArena::HostCallSafepoint;
    JitCodeArena::ArenaReadLease chain_lease{
        JitCodeArena::ArenaReadLease::StableExecution{}};
    ASSERT_TRUE(static_cast<bool>(chain_lease));
    Safepoint safepoint{&chain_lease};
    safepoint.park();
    EXPECT_TRUE(safepoint.parked());
    EXPECT_FALSE(static_cast<bool>(chain_lease));
    EXPECT_EQ(safepoint.unpark(), Safepoint::Resume::Continue);
    EXPECT_FALSE(safepoint.parked());
    EXPECT_TRUE(static_cast<bool>(chain_lease));
    // Idempotent: a second unpark reports the retained outcome.
    EXPECT_EQ(safepoint.unpark(), Safepoint::Resume::Continue);
    EXPECT_EQ(safepoint.resume(), Safepoint::Resume::Continue);

    // A closed gate on return is reported, never silently treated as held.
    Safepoint gated{&chain_lease};
    gated.park();
    JitCodeArena::request_arena_reclaim();
    EXPECT_EQ(gated.unpark(), Safepoint::Resume::LeaseLost);
    EXPECT_FALSE(static_cast<bool>(chain_lease));
    EXPECT_TRUE(JitCodeArena::reclaim_arena_if_requested());

    // Nothing to park: a null or unheld lease is a no-op safepoint.
    Safepoint none{nullptr};
    none.park();
    EXPECT_FALSE(none.parked());
    EXPECT_EQ(none.unpark(), Safepoint::Resume::Continue);
}

// A host call may re-enter the guest (WndProc from a message pump, an APC
// from an alertable wait), and that nested dispatcher opens its own chain
// lease while the outer chain's lease is parked. The nested chain parks for
// its own host calls too. The contract through all of it: the thread holds
// no lease at any instant it is inside a host call, a reclaim can therefore
// retire code while both levels are parked, and every level learns of the
// retirement when it unparks — the outer chain must not continue into a
// compiled pointer it cached before the nested callback ran.
TEST_F(JitLinearTest, HostCallSafepointNestsAcrossReentrantGuestCallback) {
    using Safepoint = JitCodeArena::HostCallSafepoint;
    using Lease = JitCodeArena::ArenaReadLease;
    std::vector<u8> code = {0xB8, 0x78, 0x56, 0x34, 0x12};
    ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
    BasicBlock block = build_block(code);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    const u64 compiled_generation = compiled->arena_generation;

    // Outer dispatch chain, parked for a host call.
    Lease outer{Lease::StableExecution{}};
    ASSERT_TRUE(static_cast<bool>(outer));
    Safepoint outer_sp{&outer};
    outer_sp.park();
    EXPECT_FALSE(Lease::thread_holds());

    // The host call re-enters the guest: a nested chain takes a fresh lease.
    // It is a real acquire, not a nesting increment, because the outer lease
    // is parked — so the nested chain's own safepoint must retract it again.
    Lease nested{Lease::StableExecution{}};
    ASSERT_TRUE(static_cast<bool>(nested));
    EXPECT_TRUE(Lease::thread_holds());
    Safepoint nested_sp{&nested};
    nested_sp.park();
    EXPECT_FALSE(Lease::thread_holds())
        << "nested chain kept the thread in the arena during its host call";

    // Both levels parked: the reclaim must be able to retire the slot.
    const usize max_advances =
        JitCodeArena::arena_budget() / JitCodeArena::arena_capacity() + 4;
    usize advances = 0;
    bool drained = true;
    while (drained &&
           JitCodeArena::arena_min_generation() <= compiled_generation &&
           advances < max_advances) {
        JitCodeArena::request_arena_reclaim();
        drained = JitCodeArena::reclaim_arena_if_requested();
        ++advances;
    }
    ASSERT_TRUE(drained) << "a doubly-parked thread still pinned the arena";
    ASSERT_GT(JitCodeArena::arena_min_generation(), compiled_generation);

    // Nested chain returns from its host call and sees the retirement.
    EXPECT_EQ(nested_sp.unpark(), Safepoint::Resume::Redispatch);
    EXPECT_TRUE(static_cast<bool>(nested));
    EXPECT_TRUE(Lease::thread_holds());
    nested.release();
    EXPECT_FALSE(Lease::thread_holds())
        << "nested release must not leave the outer (parked) level counted";

    // Outer chain returns from the host call that ran the callback. Its
    // horizon predates the retirement, so it must redispatch as well, even
    // though nothing changed between the nested unpark and here.
    EXPECT_EQ(outer_sp.unpark(), Safepoint::Resume::Redispatch)
        << "outer chain would continue into a compiled pointer cached before "
           "the callback, across a retirement it never observed";
    EXPECT_TRUE(static_cast<bool>(outer));
    EXPECT_TRUE(Lease::thread_holds());
    outer.release();
    EXPECT_FALSE(Lease::thread_holds());
}

// A thread that took a lease and exited must return its occupancy slot, or the
// shard pool leaks and a later reclaim either stalls forever or spills every
// thread onto the shared overflow counter.
TEST_F(JitLinearTest, ExitedLeaseHoldersDoNotStallReclaim) {
    // More short-lived threads than there are occupancy slots.
    for (int i = 0; i < 700; ++i) {
        std::thread([] {
            JitCodeArena::ArenaReadLease lease{
                JitCodeArena::ArenaReadLease::StableExecution{}};
            ASSERT_TRUE(static_cast<bool>(lease));
        }).join();
    }
    JitCodeArena::request_arena_reclaim();
    const auto start = std::chrono::steady_clock::now();
    EXPECT_TRUE(JitCodeArena::reclaim_arena_if_requested());
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                  .count(),
              1000);
}

// Each compiler bump-allocates inside its own chunk: blocks compiled
// concurrently by different threads never share a chunk, and every one of
// them executes correctly.
TEST_F(JitLinearTest, PerThreadChunkAllocationDoesNotInterfere) {
    constexpr int kThreads = 6;
    constexpr int kBlocksPerThread = 64;
    // Distinct guest addresses per block: recompiling one address over and
    // over trips the SMC churn guard, which is not what this test is about.
    const std::vector<u8> code = {0xB8, 0x78, 0x56, 0x34, 0x12};
    auto block_addr = [](int t, int i) {
        return kCode + static_cast<GuestAddr>((t * kBlocksPerThread + i) * 8);
    };
    for (int t = 0; t < kThreads; ++t) {
        for (int i = 0; i < kBlocksPerThread; ++i) {
            ASSERT_TRUE(m_memory->write_bytes(block_addr(t, i), code.data(),
                                              code.size()));
        }
    }
    auto build_block_at = [&](GuestAddr addr) {
        BasicBlock b = build_block(code);
        b.start_addr = addr;
        return b;
    };
    // Clear the pool of chunks with room so every compiler must take a
    // fresh one.
    m_jit->release_arena_chunk(/*seal=*/true);

    std::array<usize, kThreads> chunk_of{};
    std::array<bool, kThreads> ok{};
    std::atomic<int> started{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            BlockCache cache;
            JitCompiler jit(*m_memory);
            jit.set_block_cache(&cache);
            started.fetch_add(1, std::memory_order_acq_rel);
            while (started.load(std::memory_order_acquire) != kThreads) {
                std::this_thread::yield();
            }
            usize chunk = ~usize{0};
            bool all_ok = true;
            for (int i = 0; i < kBlocksPerThread; ++i) {
                BasicBlock block = build_block_at(block_addr(t, i));
                CompiledBlock* compiled = jit.compile(block);
                if (compiled == nullptr) {
                    ADD_FAILURE() << "thread " << t << ": compile " << i
                                  << " returned null (deferred="
                                  << jit.compile_deferred() << ")";
                    all_ok = false;
                    break;
                }
                const usize c = JitCodeArena::arena_chunk_index(compiled->code);
                if (chunk == ~usize{0}) chunk = c;
                if (c != chunk) {
                    ADD_FAILURE() << "thread " << t << ": block " << i
                                  << " landed in chunk " << c << ", not "
                                  << chunk;
                    all_ok = false;
                    break;
                }
                CpuState cpu;
                init_cpu(cpu);
                cpu.rip = block_addr(t, i);
                const JitExecResult result = jit.execute(*compiled, cpu);
                if (result != JitExecResult::Continue ||
                    cpu.gpr.get32(EAX) != 0x12345678u) {
                    ADD_FAILURE() << "thread " << t << ": block " << i
                                  << " result=" << static_cast<int>(result)
                                  << " eax=0x" << std::hex
                                  << cpu.gpr.get32(EAX);
                    all_ok = false;
                    break;
                }
            }
            chunk_of[static_cast<usize>(t)] = chunk;
            ok[static_cast<usize>(t)] = all_ok;
            // Seal so no later taker resumes it; the assertion below is
            // about distinctness at the time the code was emitted.
            jit.release_arena_chunk(/*seal=*/true);
        });
    }
    for (auto& t : threads) t.join();
    for (int t = 0; t < kThreads; ++t) {
        EXPECT_TRUE(ok[static_cast<usize>(t)]) << "thread " << t;
        for (int u = t + 1; u < kThreads; ++u) {
            EXPECT_NE(chunk_of[static_cast<usize>(t)],
                      chunk_of[static_cast<usize>(u)])
                << "threads " << t << " and " << u << " shared a chunk";
        }
    }
}

// A cold chunk retires while other threads keep executing elsewhere: no
// drain, no gate, and each running thread sees at most one StaleCode per
// retirement (the purge that re-validates its cache), never a storm. The
// bytes are recycled while those threads are still running, because their
// dispatcher entries move their published epoch forward.
TEST_F(JitLinearTest, ColdChunkRetiresWhileOtherThreadsKeepExecuting) {
    constexpr int kWorkers = 4;
    std::vector<u8> code = {0xB8, 0x78, 0x56, 0x34, 0x12};
    ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
    // The cold block, alone in its own (sealed) chunk.
    BasicBlock cold_block = build_block(code);
    CompiledBlock* cold_compiled = m_jit->compile(cold_block);
    ASSERT_NE(cold_compiled, nullptr);
    const CompiledBlock cold = *cold_compiled;
    m_jit->release_arena_chunk(/*seal=*/true);

    std::atomic<bool> stop{false};
    std::atomic<int> ready{0};
    struct WorkerResult { u64 executed{0}; u64 stale{0}; u64 wrong{0}; };
    std::array<WorkerResult, kWorkers> results{};
    std::vector<std::thread> workers;
    for (int w = 0; w < kWorkers; ++w) {
        workers.emplace_back([&, w] {
            BlockCache cache;
            JitCompiler jit(*m_memory);
            jit.set_block_cache(&cache);
            BasicBlock block = build_block(code);
            CompiledBlock* compiled = jit.compile(block);
            ASSERT_NE(compiled, nullptr);
            CompiledBlock hot = *compiled;
            WorkerResult& r = results[static_cast<usize>(w)];
            // A dispatch chain that never ends: the lease is held for the
            // whole run, like a worker thread in a tight guest loop.
            JitCodeArena::ArenaReadLease chain{
                JitCodeArena::ArenaReadLease::StableExecution{}};
            ASSERT_TRUE(static_cast<bool>(chain));
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (!stop.load(std::memory_order_acquire)) {
                CpuState cpu;
                init_cpu(cpu);
                const JitExecResult result = jit.execute(hot, cpu);
                if (result == JitExecResult::StaleCode) {
                    ++r.stale;
                    continue;
                }
                ++r.executed;
                if (cpu.gpr.get32(EAX) != 0x12345678u) ++r.wrong;
            }
        });
    }
    while (ready.load(std::memory_order_acquire) != kWorkers) {
        std::this_thread::yield();
    }

    const u64 refusals_before = JitCodeArena::arena_gate_refusals();
    const u64 frees_before = JitCodeArena::arena_chunk_frees();
    const u64 retirements_before = JitCodeArena::arena_chunk_retirements();
    // Retire until the cold block's chunk is gone. Workers own their chunks,
    // so non-forced selection never takes theirs.
    usize rounds = 0;
    while (JitCodeArena::arena_code_live(cold.code, cold.arena_generation) &&
           rounds < JitCodeArena::arena_chunk_count()) {
        ASSERT_EQ(JitCodeArena::retire_arena_chunks(1, /*force=*/false), 1u);
        ++rounds;
    }
    const u64 retirements =
        JitCodeArena::arena_chunk_retirements() - retirements_before;
    ASSERT_FALSE(JitCodeArena::arena_code_live(cold.code, cold.arena_generation));

    // The workers never leave their chains, yet the retired bytes must be
    // recycled: each execute() re-publishes the thread's epoch.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (JitCodeArena::arena_pending_free_count() != 0 &&
           std::chrono::steady_clock::now() < deadline) {
        (void)JitCodeArena::sweep_retired_chunks();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(JitCodeArena::arena_pending_free_count(), 0u)
        << "retired chunks stayed pending behind threads that never stop";
    EXPECT_GE(JitCodeArena::arena_chunk_frees() - frees_before, retirements);
    EXPECT_FALSE(JitCodeArena::arena_reclaim_pending());
    EXPECT_EQ(JitCodeArena::arena_gate_refusals(), refusals_before);

    stop.store(true, std::memory_order_release);
    for (auto& t : workers) t.join();
    for (int w = 0; w < kWorkers; ++w) {
        const WorkerResult& r = results[static_cast<usize>(w)];
        EXPECT_EQ(r.wrong, 0u) << "worker " << w;
        EXPECT_GT(r.executed, 0u) << "worker " << w;
        EXPECT_LE(r.stale, retirements)
            << "worker " << w << " saw a StaleCode storm";
    }
}

}  // namespace
