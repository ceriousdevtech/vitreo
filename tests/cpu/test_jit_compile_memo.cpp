// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT execution tests — compile memo and per-compile state isolation

#include "jit_exec_test_fixture.hpp"
#include "core/cpu/jit/jit_compile_test_hooks.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <mutex>

TEST_F(JitExecTest, CompileMemoDistinguishesCodeAtSameAddress) {
    struct MemoGuard {
        ~MemoGuard() {
            JitCompileMemo::disable_compile_memo();
            JitCompileMemo::clear_compile_memo();
        }
    } memo_guard;

    JitCompileMemo::clear_compile_memo();
    JitCompileMemo::enable_compile_memo();

    const u8 first_code[] = {0xB8, 0x01, 0x00, 0x00, 0x00};
    BasicBlock first_block = build_block(first_code, sizeof(first_code));
    ASSERT_NE(m_jit->compile(first_block), nullptr);

    const u8 second_code[] = {0xB8, 0x02, 0x00, 0x00, 0x00};
    BasicBlock second_block = build_block(second_code, sizeof(second_code));
    CompiledBlock* second_compiled = m_jit->compile(second_block);
    ASSERT_NE(second_compiled, nullptr);

    m_cpu->gpr.set32(EAX, 0);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *second_compiled, *m_cpu),
              JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 2u);
}

// A block whose stencils delegate to the interpreter embeds
// `&instructions[i]` — the only thread-owned pointer in generated code. The
// compile memo shares such a block by owning that decoded array itself, so the
// shared arena bytes stay correct after every other holder is gone. This is
// the whole point of sharing them: on a CEF-style startup, delegate-bearing
// blocks are a large fraction of all compiles, and excluding them means every
// thread re-emits them.
TEST_F(JitExecTest, CompileMemoSharesDelegateBlocksAndOutlivesTheirDecoder) {
    struct MemoGuard {
        ~MemoGuard() {
            JitCompileMemo::disable_compile_memo();
            JitCompileMemo::clear_compile_memo();
        }
    } memo_guard;

    JitCompileMemo::clear_compile_memo();
    JitCompileMemo::enable_compile_memo();

    // CPUID has no native stencil, so it compiles to a pre-decoded delegate.
    const u8 code[] = {0x0F, 0xA2};

    void* first_code = nullptr;
    u32 expected_eax = 0, expected_ebx = 0, expected_ecx = 0, expected_edx = 0;
    u64 expected_rip = 0;
    {
        JitCompiler first_jit(*m_memory);
        BasicBlock first_block = build_block(code, sizeof(code));
        CompiledBlock* first_compiled = first_jit.compile(first_block);
        ASSERT_NE(first_compiled, nullptr);
        first_code = first_compiled->code;

        m_cpu->gpr.set32(EAX, 0);
        m_cpu->rip = 0x1000;
        ASSERT_EQ(test::execute_like_dispatcher(first_jit, *first_compiled, *m_cpu),
                  JitExecResult::Continue);
        expected_eax = m_cpu->gpr.get32(EAX);
        expected_ebx = m_cpu->gpr.get32(EBX);
        expected_ecx = m_cpu->gpr.get32(ECX);
        expected_edx = m_cpu->gpr.get32(EDX);
        expected_rip = m_cpu->rip;
        // Drop every reference this thread holds to the decoded array: the
        // block's own handle, the compiler, and its compiled records. Only the
        // memo can still be keeping the delegate's target alive.
        first_block.instructions.reset();
    }

    JitCompiler second_jit(*m_memory);
    BasicBlock second_block = build_block(code, sizeof(code));
    CompiledBlock* second_compiled = second_jit.compile(second_block);
    ASSERT_NE(second_compiled, nullptr);
    EXPECT_EQ(second_compiled->code, first_code)
        << "delegate-bearing block was re-emitted instead of shared";
    ASSERT_NE(second_compiled->delegate_insns, nullptr)
        << "shared delegate code must carry the array it dereferences";

    m_cpu->gpr.set32(EAX, 0);
    m_cpu->rip = 0x1000;
    ASSERT_EQ(test::execute_like_dispatcher(second_jit, *second_compiled, *m_cpu),
              JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), expected_eax);
    EXPECT_EQ(m_cpu->gpr.get32(EBX), expected_ebx);
    EXPECT_EQ(m_cpu->gpr.get32(ECX), expected_ecx);
    EXPECT_EQ(m_cpu->gpr.get32(EDX), expected_edx);
    EXPECT_EQ(m_cpu->rip, expected_rip);

    // publish_compiled must hand the shared array to the reusing thread's
    // cache entry, or that entry's own decode (a distinct allocation) would
    // be the only thing keeping a now-unrelated array alive.
    publish_compiled(second_block, *second_compiled);
    EXPECT_EQ(second_block.instructions.share(),
              second_compiled->delegate_insns);
}

TEST_F(JitExecTest, CompileMemoClearReleasesDelegateIrAccounting) {
    struct MemoGuard {
        ~MemoGuard() {
            JitCompileMemo::disable_compile_memo();
            JitCompileMemo::clear_compile_memo();
        }
    } memo_guard;

    JitCompileMemo::clear_compile_memo();
    JitCompileMemo::enable_compile_memo();
    ASSERT_EQ(memo_ir_bytes(), 0u);

    const u8 code[] = {0x0F, 0xA2};  // CPUID — delegate-bearing
    BasicBlock block = build_block(code, sizeof(code));
    ASSERT_NE(m_jit->compile(block), nullptr);
    EXPECT_GT(memo_ir_bytes(), 0u);

    JitCompileMemo::clear_compile_memo();
    EXPECT_EQ(memo_ir_bytes(), 0u);
}

// Real threads, not two compilers on one thread: the memo is read under a
// shared lock and written under a unique one, and a delegate record hands out
// a shared_ptr, so the racing paths (publish vs. reuse, and the losing side of
// an emplace race giving its IR reservation back) only run concurrently here.
TEST_F(JitExecTest, CompileMemoSharesOneTranslationUnderConcurrentCompiles) {
    struct MemoGuard {
        ~MemoGuard() {
            JitCompileMemo::disable_compile_memo();
            JitCompileMemo::clear_compile_memo();
        }
    } memo_guard;

    JitCompileMemo::clear_compile_memo();
    JitCompileMemo::enable_compile_memo();

    const u8 code[] = {0x0F, 0xA2};  // CPUID — delegate-bearing
    auto wr = m_memory->write_bytes(0x1000, code, sizeof(code));
    ASSERT_TRUE(wr);

    // Seed the memo first. Threads that reach an *uncompiled* block at the
    // same instant now coalesce before backend emission (covered separately
    // below). This seeded case exercises the memo-hit path without any
    // in-flight claim lookup.
    void* seeded = nullptr;
    {
        JitCompiler seed_jit(*m_memory);
        BasicBlock seed_block = build_block(code, sizeof(code));
        CompiledBlock* seed_compiled = seed_jit.compile(seed_block);
        ASSERT_NE(seed_compiled, nullptr);
        seeded = seed_compiled->code;
        seed_block.instructions.reset();
    }

    constexpr int kThreads = 8;
    std::array<void*, kThreads> codes{};
    std::array<u32, kThreads> eax{};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            JitCompiler jit(*m_memory);
            BasicBlock block = build_block(code, sizeof(code));
            CompiledBlock* compiled = jit.compile(block);
            if (compiled == nullptr) return;
            codes[static_cast<usize>(t)] = compiled->code;

            CpuState cpu;
            cpu.init_protected_mode();
            cpu.gpr.set32(ESP, 0x10000);
            cpu.rip = 0x1000;
            cpu.gpr.set32(EAX, 0);
            if (test::execute_like_dispatcher(jit, *compiled, cpu) == JitExecResult::Continue) {
                eax[static_cast<usize>(t)] = cpu.gpr.get32(EAX);
            }
            // Release this thread's own decode; the shared code must not
            // depend on it.
            block.instructions.reset();
        });
    }
    for (auto& th : threads) th.join();

    for (int t = 0; t < kThreads; ++t) {
        ASSERT_NE(codes[static_cast<usize>(t)], nullptr) << "thread " << t;
        EXPECT_EQ(codes[static_cast<usize>(t)], seeded)
            << "thread " << t << " emitted its own copy of a shared block";
        EXPECT_EQ(eax[static_cast<usize>(t)], eax[0]) << "thread " << t;
    }
    EXPECT_NE(eax[0], 0u) << "CPUID delegate did not run";
}

// Cross-thread invalidation: guest code that changes underneath a shared block
// must never be served the stale translation, in any thread. The memo key
// carries the block's byte identity, so changed bytes are a different key —
// and code that reverts to its original bytes legitimately reuses the original
// translation.
TEST_F(JitExecTest, CompileMemoNeverServesSharedCodeForChangedGuestBytes) {
    struct MemoGuard {
        ~MemoGuard() {
            JitCompileMemo::disable_compile_memo();
            JitCompileMemo::clear_compile_memo();
        }
    } memo_guard;

    JitCompileMemo::clear_compile_memo();
    JitCompileMemo::enable_compile_memo();

    // Delegate-bearing (CPUID) then rewritten in place to a different
    // delegate-bearing instruction of the same length (RDTSC).
    const u8 cpuid[] = {0x0F, 0xA2};
    const u8 rdtsc[] = {0x0F, 0x31};

    JitCompiler first_jit(*m_memory);
    BasicBlock cpuid_block = build_block(cpuid, sizeof(cpuid));
    CompiledBlock* cpuid_compiled = first_jit.compile(cpuid_block);
    ASSERT_NE(cpuid_compiled, nullptr);
    void* const cpuid_code = cpuid_compiled->code;

    // A second thread reaching the SAME guest address after the bytes changed
    // must compile the new instruction, not inherit the old arena code.
    JitCompiler second_jit(*m_memory);
    BasicBlock rdtsc_block = build_block(rdtsc, sizeof(rdtsc));
    CompiledBlock* rdtsc_compiled = second_jit.compile(rdtsc_block);
    ASSERT_NE(rdtsc_compiled, nullptr);
    EXPECT_NE(rdtsc_compiled->code, cpuid_code);

    m_cpu->gpr.set32(EAX, 0);
    m_cpu->gpr.set32(EDX, 0);
    m_cpu->rip = 0x1000;
    ASSERT_EQ(test::execute_like_dispatcher(second_jit, *rdtsc_compiled, *m_cpu),
              JitExecResult::Continue);
    // RDTSC writes EDX:EAX from the guest TSC; CPUID leaf 0 would have written
    // a vendor string into EBX/ECX/EDX instead. EBX untouched proves the block
    // that ran was the recompiled one.
    EXPECT_EQ(m_cpu->gpr.get32(EBX), 0u);

    // Reverting the bytes must resolve back to the original shared code: the
    // key is byte identity, not a monotonically rising version.
    JitCompiler third_jit(*m_memory);
    BasicBlock reverted = build_block(cpuid, sizeof(cpuid));
    CompiledBlock* reverted_compiled = third_jit.compile(reverted);
    ASSERT_NE(reverted_compiled, nullptr);
    EXPECT_EQ(reverted_compiled->code, cpuid_code);
}

TEST_F(JitExecTest, CompilationDoesNotLeakNzcvAcrossBlocks) {
    // Compilation order must not affect a block's generated code. In
    // particular, an ADD compiled immediately beforehand must not make the
    // first ADC in an unrelated block consume ARM NZCV: runtime dispatch does
    // not preserve those host flags across guest basic-block boundaries.
    // Contiguous placement: a split block's hot-to-cold branches encode where
    // its cold part landed, which differs between the two compilers below for
    // reasons unrelated to compilation order.
    struct NoSplit {
        NoSplit() { setenv("VITREO_JIT_SPLIT_COLD", "0", 1); }
        ~NoSplit() { unsetenv("VITREO_JIT_SPLIT_COLD"); }
    } no_split;
    JitCompiler reused_jit(*m_memory);
    const u8 producer_code[] = {0x01, 0xD8};  // add eax,ebx
    BasicBlock producer = build_block(producer_code, sizeof(producer_code));
    ASSERT_NE(reused_jit.compile(producer), nullptr);

    const u8 consumer_code[] = {0x11, 0xD1};  // adc ecx,edx
    BasicBlock reused_block = build_block(consumer_code, sizeof(consumer_code));
    CompiledBlock* reused = reused_jit.compile(reused_block);
    ASSERT_NE(reused, nullptr);

    JitCompiler fresh_jit(*m_memory);
    BasicBlock fresh_block = build_block(consumer_code, sizeof(consumer_code));
    CompiledBlock* fresh = fresh_jit.compile(fresh_block);
    ASSERT_NE(fresh, nullptr);

    ASSERT_EQ(reused->code_size, fresh->code_size);
    EXPECT_EQ(std::memcmp(reused->code, fresh->code, fresh->code_size), 0);
}

TEST_F(JitExecTest, CompileSnapshotsCachedBlockBeforeReentrantGuiPump) {
    const u8 original_code[] = {
        0xF2, 0x0F, 0x2A, 0xC1, // cvtsi2sd xmm0,ecx
    };
    auto original = build_block(original_code, sizeof(original_code));

    const u8 replacement_code[] = {
        0xF2, 0x0F, 0x2A, 0xC2, // cvtsi2sd xmm0,edx
    };
    BasicBlock replacement;
    replacement.start_addr = 0x1000;
    MemoryByteReader replacement_reader(
        replacement_code, sizeof(replacement_code));
    while (replacement_reader.remaining() > 0) {
        DecodedInstruction instruction;
        if (decode_instruction(replacement_reader, DecodeMode::Mode32,
                               instruction) != DecodeError::Success) {
            break;
        }
        replacement.instructions.push_back(instruction);
    }
    replacement.byte_length =
        static_cast<u32>(replacement_reader.position());
    replacement.instruction_count =
        static_cast<u32>(replacement.instructions.size());

    BlockCache cache{1};
    cache.insert(std::move(original));
    BasicBlock* cached = cache.lookup(0x1000);
    ASSERT_NE(cached, nullptr);

    bool pumped = false;
    m_jit->set_gui_pump([&] {
        if (pumped) return;
        pumped = true;
        // Updating the same address mutates the object referenced by cached.
        cache.insert(std::move(replacement));
    });

    // Compilation polls the GUI every 16 blocks to avoid a host-clock query
    // on every short compile. Prime the counter with distinct addresses so
    // recompile-demotion and memo hits do not obscure the snapshot behavior.
    for (u32 i = 0; i < 15; ++i) {
        const u8 primer_code[] = {
            0xB8, static_cast<u8>(i), 0x00, 0x00, 0x00, // mov eax,i
        };
        BasicBlock primer = build_block(primer_code, sizeof(primer_code));
        primer.start_addr = 0x3000 + i * 0x10;
        ASSERT_NE(m_jit->compile(primer), nullptr);
    }
    CompiledBlock* compiled = m_jit->compile(*cached);
    ASSERT_TRUE(pumped);
    ASSERT_NE(compiled, nullptr);

    m_cpu->rip = 0x1000;
    m_cpu->gpr.set32(ECX, 42);
    m_cpu->gpr.set32(EDX, 7);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, *m_cpu), JitExecResult::Continue);
    EXPECT_DOUBLE_EQ(m_cpu->sse.xmm[0].f64_[0], 42.0);
}

namespace {
struct ColdCompileMemoGuard {
    ColdCompileMemoGuard() {
        JitCompileMemo::clear_compile_memo();
        JitCompileMemo::enable_compile_memo();
        JitCompileMemo::reset_compile_memo_stats();
    }
    ~ColdCompileMemoGuard() {
        JitCompileMemo::disable_compile_memo();
        JitCompileMemo::clear_compile_memo();
    }
};

template<class Predicate>
bool spin_until(Predicate ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!ready()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

bool coalescing_disabled() {
    return !environment_flag_enabled("VITREO_JIT_COALESCE_COMPILES", false);
}
}

TEST_F(JitExecTest, CompileMemoCoalescesColdNativeAndDelegateMissesInBothModes) {
    if (coalescing_disabled()) GTEST_SKIP() << "coalescing diagnostic control disabled";
    ColdCompileMemoGuard memo;
    constexpr usize workers = 8;
    for (const bool wide : {false, true}) {
        for (const bool delegate : {false, true}) {
            SCOPED_TRACE(::testing::Message() << "wide=" << wide << " delegate=" << delegate);
            std::vector<u8> code;
            for (int i = 0; i < 64; ++i) {
                if (delegate) code.insert(code.end(), {0x0F, 0xA2});
                else code.insert(code.end(), {0x83, 0xC0, 0x01});
            }
            auto source = build_block(code.data(), code.size());
            source.is_64bit = wide;
            JitCompileMemo::clear_compile_memo();
            JitCompileMemo::reset_compile_memo_stats();
            std::mutex gate_mutex;
            std::condition_variable gate;
            usize ready = 0;
            bool go = false;
            std::array<void*, workers> pointers{};
            std::array<bool, workers> executed{};
            std::array<u32, workers> results{};
            std::array<bool, workers> owns_decode{};
            std::vector<std::thread> threads;
            for (usize t = 0; t < workers; ++t) {
                threads.emplace_back([&, t] {
                    JitCompiler compiler(*m_memory);
                    BasicBlock block = source;
                    {
                        std::unique_lock lock(gate_mutex);
                        ++ready;
                        gate.notify_all();
                        gate.wait(lock, [&] { return go; });
                    }
                    auto* compiled = compiler.compile(block);
                    if (!compiled) return;
                    pointers[t] = compiled->code;
                    owns_decode[t] = compiled->delegate_insns != nullptr;
                    CpuState cpu;
                    if (wide) cpu.init_long_mode(); else cpu.init_protected_mode();
                    cpu.gpr.set64(ESP, 0x10000);
                    cpu.rip = source.start_addr;
                    executed[t] = test::execute_like_dispatcher(compiler, *compiled, cpu) ==
                                  JitExecResult::Continue;
                    results[t] = cpu.gpr.get32(EAX);
                    block.instructions.reset();
                });
            }
            {
                std::unique_lock lock(gate_mutex);
                gate.wait(lock, [&] { return ready == workers; });
                go = true;
            }
            gate.notify_all();
            for (auto& thread : threads) thread.join();
            for (usize t = 0; t < workers; ++t) {
                ASSERT_NE(pointers[t], nullptr) << t;
                EXPECT_EQ(pointers[t], pointers[0]) << t;
                EXPECT_TRUE(executed[t]) << t;
                EXPECT_EQ(results[t], results[0]) << t;
                EXPECT_EQ(owns_decode[t], delegate) << t;
            }
            if (!delegate) EXPECT_EQ(results[0], 64u);
            EXPECT_EQ(JitCompileMemo::compile_memo_stats().fresh_emits, 1u);
            EXPECT_EQ(JitCompileMemo::compile_memo_size(), 1u);
            EXPECT_EQ(JitCompileMemo::compile_waiters_for_test(), 0u);
        }
    }
}

TEST_F(JitExecTest, CompileClaimReleasesAbandonedWorkAndRejectsWaitedSourceChanges) {
    if (coalescing_disabled()) GTEST_SKIP() << "coalescing diagnostic control disabled";
    ColdCompileMemoGuard memo;
    for (const bool change_source : {false, true}) {
        SCOPED_TRACE(change_source);
        const u8 code[] = {0xB8, 0x01, 0x00, 0x00, 0x00};
        auto source = build_block(code, sizeof(code));
        JitCompileMemo::clear_compile_memo();
        JitCompiler seed(*m_memory);
        ASSERT_NE(seed.compile(source), nullptr);
        JitCompileMemo::CompiledKey key{};
        {
            auto& shard = JitCompileMemo::shard_for(source.start_addr);
            std::shared_lock lock(shard.lock);
            const auto found = shard.compiled.find(source.start_addr);
            ASSERT_NE(found, shard.compiled.end());
            found->second.for_each([&](const auto& entry) { key = entry.key; });
        }
        JitCompileMemo::clear_compile_memo();
        JitCompileMemo::reset_compile_memo_stats();
        auto abandoned = std::make_unique<JitCompileMemo::CompilationClaim>();
        abandoned->acquire(*m_memory, key, source.instruction_count,
                           m_memory->executable_generation());
        ASSERT_TRUE(abandoned->owns_claim());
        std::atomic<bool> returned{false};
        bool succeeded = false, deferred = false, changed = false;
        bool retry_succeeded = false, retry_cleared = false;
        std::thread follower([&] {
            JitCompiler compiler(*m_memory);
            succeeded = compiler.compile(source) != nullptr;
            deferred = compiler.compile_deferred();
            changed = compiler.compile_source_changed();
            if (changed) {
                BasicBlock refreshed = source;
                // MOV EAX,imm32 retains the same shape; decode its new bytes.
                const u8 updated[] = {0xB8, 0x02, 0x00, 0x00, 0x00};
                MemoryByteReader reader(updated, sizeof(updated));
                DecodedInstruction instruction;
                if (decode_instruction(reader, DecodeMode::Mode32, instruction) == DecodeError::Success) {
                    refreshed.instructions.reset();
                    refreshed.instructions.push_back(instruction);
                    retry_succeeded = compiler.compile(refreshed) != nullptr;
                    retry_cleared = !compiler.compile_source_changed() && !compiler.compile_deferred();
                }
            }
            returned.store(true, std::memory_order_release);
        });
        const bool blocked = spin_until([&] {
            return JitCompileMemo::compile_waiters_for_test() != 0 ||
                   returned.load(std::memory_order_acquire);
        }) && !returned.load(std::memory_order_acquire);
        if (change_source && blocked) {
            const u8 updated[] = {0xB8, 0x02, 0x00, 0x00, 0x00};
            EXPECT_TRUE(m_memory->write_bytes(source.start_addr, updated, sizeof(updated)));
            m_memory->notify_executable_write(0, 0);
        }
        abandoned.reset(); // No publication: a follower must become the owner.
        follower.join();
        ASSERT_TRUE(blocked);
        EXPECT_EQ(succeeded, !change_source);
        EXPECT_EQ(deferred, change_source);
        EXPECT_EQ(changed, change_source);
        if (change_source) {
            EXPECT_TRUE(retry_succeeded);
            EXPECT_TRUE(retry_cleared);
        }
        EXPECT_EQ(JitCompileMemo::compile_memo_stats().fresh_emits, 1u);
        EXPECT_EQ(JitCompileMemo::compile_waiters_for_test(), 0u);
    }
}

TEST_F(JitExecTest, CompileClaimsIsolateIdentityAndNeverWaitDuringNestedCompilation) {
    JitCompileMemo::CompiledKey original{};
    original.start_addr = 0x1000;
    original.guest_size = 1;
    original.code_hash = 1;
    constexpr u64 generation = 7;
    auto leader = std::make_unique<JitCompileMemo::CompilationClaim>();
    leader->acquire(*m_memory, original, 1, generation);
    ASSERT_TRUE(leader->owns_claim());
    {
        JitCompileMemo::CompilationClaim nested;
        nested.acquire(*m_memory, original, 1, generation);
        EXPECT_FALSE(nested.owns_claim());
        EXPECT_FALSE(nested.waited());
    }
    for (int variant = 0; variant < 9; ++variant) {
        auto key = original;
        GuestMemory other_memory;
        auto* memory = m_memory.get();
        u64 epoch = generation;
        u32 count = 1;
        switch (variant) {
        case 0: key.start_addr += 1; break; // Same registry shard, disjoint key.
        case 1: key.is_64bit = true; break;
        case 2: key.code_hash += 1; break;
        case 3: memory = &other_memory; break;
        case 4: ++epoch; break;
        case 5: ++count; break;
        case 6: ++key.ir_policy; break;
        case 7: ++key.pin_layout; break;
        case 8: ++key.guest_size; break;
        }
        std::atomic<bool> acquired{false};
        std::thread independent([&] {
            JitCompileMemo::CompilationClaim claim;
            claim.acquire(*memory, key, count, epoch);
            acquired.store(claim.owns_claim(), std::memory_order_release);
        });
        // These must finish while the original claim remains live. A broken
        // identity would wait forever, so release it on timeout before join.
        const bool progressed = spin_until([&] { return acquired.load(std::memory_order_acquire); });
        if (!progressed) leader.reset();
        independent.join();
        ASSERT_TRUE(progressed) << "independent claim blocked for variant " << variant;
    }
}

TEST_F(JitExecTest, CompileClaimFansOutUnshareableAndFailedBackendsWithoutLeakingClaims) {
    if (coalescing_disabled()) GTEST_SKIP() << "coalescing diagnostic control disabled";
    ColdCompileMemoGuard memo;
    constexpr usize workers = 8;
    for (const bool exhausted_budget : {true, false}) {
        SCOPED_TRACE(exhausted_budget);
        const std::vector<u8> code = exhausted_budget
            ? std::vector<u8>{0x0F, 0xA2}
            : std::vector<u8>{0x8B, 0x03, 0x90};
        auto source = build_block(code.data(), code.size());
        JitCompileMemo::clear_compile_memo();
        JitCompiler seed(*m_memory);
        ASSERT_NE(seed.compile(source), nullptr);
        JitCompileMemo::CompiledKey key{};
        {
            auto& shard = JitCompileMemo::shard_for(source.start_addr);
            std::shared_lock lock(shard.lock);
            const auto found = shard.compiled.find(source.start_addr);
            ASSERT_NE(found, shard.compiled.end());
            found->second.for_each([&](const auto& entry) { key = entry.key; });
        }
        JitCompileMemo::clear_compile_memo();
        struct BudgetReservation {
            usize bytes{0};
            void release() {
                if (bytes) memo_ir_release(bytes);
                bytes = 0;
            }
            ~BudgetReservation() { release(); }
        } reservation;
        if (exhausted_budget) {
            ASSERT_EQ(memo_ir_bytes(), 0u);
            // Reserve the exact configured capacity using its admission API.
            // No guessed cap and no actual decoded-array allocation.
            for (usize chunk = usize{1} << (std::numeric_limits<usize>::digits - 1);
                 chunk != 0; chunk >>= 1) {
                if (memo_ir_admit(chunk)) reservation.bytes += chunk;
            }
            ASSERT_FALSE(memo_ir_admit(1));
        }
        struct ReachLimit {
            std::string fatal, words;
            bool had_fatal, had_words;
            explicit ReachLimit(bool enable) :
                had_fatal(std::getenv("VITREO_JIT_TEST_BRANCH_REACH_FATAL") != nullptr),
                had_words(std::getenv("VITREO_JIT_TEST_BRANCH_REACH_WORDS") != nullptr) {
                if (had_fatal) fatal = std::getenv("VITREO_JIT_TEST_BRANCH_REACH_FATAL");
                if (had_words) words = std::getenv("VITREO_JIT_TEST_BRANCH_REACH_WORDS");
                if (enable) {
                    ::setenv("VITREO_JIT_TEST_BRANCH_REACH_FATAL", "0", 1);
                    ::setenv("VITREO_JIT_TEST_BRANCH_REACH_WORDS", "1", 1);
                    JitCompiler::reload_branch_reach_test_knobs_for_test();
                }
            }
            void restore() {
                if (had_fatal) ::setenv("VITREO_JIT_TEST_BRANCH_REACH_FATAL", fatal.c_str(), 1);
                else ::unsetenv("VITREO_JIT_TEST_BRANCH_REACH_FATAL");
                if (had_words) ::setenv("VITREO_JIT_TEST_BRANCH_REACH_WORDS", words.c_str(), 1);
                else ::unsetenv("VITREO_JIT_TEST_BRANCH_REACH_WORDS");
                JitCompiler::reload_branch_reach_test_knobs_for_test();
            }
            ~ReachLimit() { restore(); }
        } reach(!exhausted_budget);
        JitCompileMemo::reset_compile_memo_stats();
        auto abandoned = std::make_unique<JitCompileMemo::CompilationClaim>();
        abandoned->acquire(*m_memory, key, source.instruction_count,
                           m_memory->executable_generation());
        ASSERT_TRUE(abandoned->owns_claim());
        std::array<bool, workers> succeeded{}, owns_decode{}, deferred{};
        std::array<void*, workers> codes{};
        std::vector<std::thread> threads;
        for (usize t = 0; t < workers; ++t) {
            threads.emplace_back([&, t] {
                JitCompiler compiler(*m_memory);
                auto* compiled = compiler.compile(source);
                succeeded[t] = compiled != nullptr;
                deferred[t] = compiler.compile_deferred();
                if (compiled) {
                    codes[t] = compiled->code;
                    owns_decode[t] = compiled->delegate_insns != nullptr;
                }
            });
        }
        const bool all_waiting = spin_until([&] {
            return JitCompileMemo::compile_waiters_for_test() == workers;
        });
        abandoned.reset();
        for (auto& thread : threads) thread.join();
        ASSERT_TRUE(all_waiting);
        for (usize t = 0; t < workers; ++t) {
            EXPECT_EQ(succeeded[t], exhausted_budget) << t;
            EXPECT_FALSE(deferred[t]) << t;
            EXPECT_EQ(owns_decode[t], exhausted_budget) << t;
            if (exhausted_budget) {
                for (usize previous = 0; previous < t; ++previous)
                    EXPECT_NE(codes[t], codes[previous]);
            }
        }
        EXPECT_EQ(JitCompileMemo::compile_memo_size(), 0u);
        EXPECT_EQ(JitCompileMemo::compile_waiters_for_test(), 0u);
        EXPECT_EQ(JitCompileMemo::compile_memo_stats().fresh_emits,
                  exhausted_budget ? workers : 0u);
        reservation.release();
        reach.restore();
        EXPECT_EQ(memo_ir_bytes(), 0u);
        JitCompiler retry(*m_memory);
        EXPECT_NE(retry.compile(source), nullptr);
        EXPECT_EQ(JitCompileMemo::compile_memo_size(), 1u);
    }
}

TEST_F(JitExecTest, CompileClaimReusesIdentityWithoutResettingSurvivingWaiters) {
    for (u64 round = 0; round < 16; ++round) {
        JitCompileMemo::CompiledKey key{};
        key.start_addr = 0x1000 + round * 0x1000;
        if (round != 0) {
            // Round transitions cross registry shards; immediate reacquisition
            // below stays in one shard. Both cache-reuse paths must work.
            ASSERT_NE(JitCompileMemo::shard_index(key.start_addr),
                      JitCompileMemo::shard_index(key.start_addr - 0x1000));
        }
        key.code_hash = round;
        key.pin_layout = round + 1;
        key.guest_size = static_cast<u32>(round + 1);
        key.ir_policy = static_cast<u32>(round);
        key.is_64bit = (round & 1) != 0;
        const u64 generation = round + 1;
        const u32 count = static_cast<u32>(round + 1);
        auto owner = std::make_unique<JitCompileMemo::CompilationClaim>();
        owner->acquire(*m_memory, key, count, generation);
        ASSERT_TRUE(owner->owns_claim());
        std::atomic<bool> returned{false};
        bool waited = false;
        std::thread follower([&] {
            JitCompileMemo::CompilationClaim claim;
            claim.acquire(*m_memory, key, count, generation);
            waited = claim.waited();
            returned.store(true, std::memory_order_release);
        });
        const bool blocked = spin_until([&] {
            return JitCompileMemo::compile_waiters_for_test() != 0 ||
                   returned.load(std::memory_order_acquire);
        }) && !returned.load(std::memory_order_acquire);
        owner.reset();
        // A new owner can arrive before the old waiter has reacquired its
        // mutex. Reusing that waiter's state would erase its completed wakeup.
        auto next = std::make_unique<JitCompileMemo::CompilationClaim>();
        next->acquire(*m_memory, key, count, generation);
        const bool next_owned = next->owns_claim();
        const bool follower_progressed = spin_until([&] {
            return returned.load(std::memory_order_acquire);
        });
        next.reset();
        follower.join();
        ASSERT_TRUE(blocked);
        ASSERT_TRUE(next_owned);
        ASSERT_TRUE(follower_progressed);
        EXPECT_TRUE(waited);
        EXPECT_EQ(JitCompileMemo::compile_waiters_for_test(), 0u);
    }
}

namespace {
struct LateMemoGate {
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> claimed{false};
    bool entered{false};
    bool released{false};
};
LateMemoGate* g_late_memo_gate = nullptr;

void pause_first_late_probe(GuestAddr address) {
    auto& gate = *g_late_memo_gate;
    if (address != 0x1000 || gate.claimed.exchange(true)) return;
    std::unique_lock lock(gate.mutex);
    gate.entered = true;
    gate.cv.notify_all();
    gate.cv.wait(lock, [&] { return gate.released; });
}
}

TEST_F(JitExecTest, LateMemoProbeAdoptsPublicationAfterFrontendBeforeAnyBackendEmission) {
    if (!coalescing_disabled()) GTEST_SKIP() << "blocking coalescer owns the key before this boundary";
    if (!environment_flag_enabled("VITREO_JIT_LATE_MEMO_RECHECK", true))
        GTEST_SKIP() << "late memo diagnostic control disabled";
    ColdCompileMemoGuard memo;
    for (const bool wide : {false, true}) {
        for (const bool delegate : {false, true}) {
            SCOPED_TRACE(::testing::Message() << "wide=" << wide << " delegate=" << delegate);
            const std::vector<u8> code = delegate
                ? std::vector<u8>{0x0F, 0xA2}
                : std::vector<u8>{0x83, 0xC0, 0x01};
            auto source = build_block(code.data(), code.size());
            source.is_64bit = wide;
            JitCompileMemo::clear_compile_memo();
            JitCompileMemo::reset_compile_memo_stats();
            LateMemoGate gate;
            g_late_memo_gate = &gate;
            compile_test::set_before_backend_hook(&pause_first_late_probe);
            void* first_code = nullptr;
            bool first_owns_decode = false;
            std::thread first([&] {
                JitCompiler compiler(*m_memory);
                if (auto* compiled = compiler.compile(source)) {
                    first_code = compiled->code;
                    first_owns_decode = compiled->delegate_insns != nullptr;
                }
            });
            bool arrived;
            {
                std::unique_lock lock(gate.mutex);
                arrived = gate.cv.wait_for(lock, std::chrono::seconds(5), [&] { return gate.entered; });
            }
            JitCompiler publisher(*m_memory);
            CompiledBlock* published = arrived ? publisher.compile(source) : nullptr;
            {
                std::lock_guard lock(gate.mutex);
                gate.released = true;
            }
            gate.cv.notify_all();
            first.join();
            compile_test::set_before_backend_hook(nullptr);
            g_late_memo_gate = nullptr;
            ASSERT_TRUE(arrived);
            ASSERT_NE(published, nullptr);
            EXPECT_EQ(first_code, published->code);
            EXPECT_EQ(first_owns_decode, delegate);
            EXPECT_EQ(JitCompileMemo::compile_memo_stats().fresh_emits, 1u);
            EXPECT_EQ(JitCompileMemo::compile_memo_stats().compile_hits, 1u);
            // First lookup + publisher lookup + the changed-sequence retry.
            EXPECT_EQ(JitCompileMemo::compile_memo_stats().compile_probes, 3u);
        }
    }
}

TEST_F(JitExecTest, LateMemoProbeSkipsUnchangedSequenceAndClearNeverResetsIt) {
    if (!coalescing_disabled()) GTEST_SKIP() << "blocking coalescer adds its own re-probe";
    if (!environment_flag_enabled("VITREO_JIT_LATE_MEMO_RECHECK", true))
        GTEST_SKIP() << "late memo diagnostic control disabled";
    ColdCompileMemoGuard memo;
    struct RestoreLockMode {
        ~RestoreLockMode() { JitCompileMemo::set_legacy_memo_locking_for_test(-1); }
    } restore;
    for (const bool legacy : {false, true}) {
        JitCompileMemo::set_legacy_memo_locking_for_test(legacy ? 1 : 0);
        for (const bool wide : {false, true}) {
            for (const bool delegate : {false, true}) {
                SCOPED_TRACE(::testing::Message() << "legacy=" << legacy << " wide=" << wide
                                                << " delegate=" << delegate);
                const std::vector<u8> code = delegate
                    ? std::vector<u8>{0x0F, 0xA2}
                    : std::vector<u8>{0x83, 0xC0, 0x01};
                auto source = build_block(code.data(), code.size());
                source.is_64bit = wide;
                JitCompileMemo::clear_compile_memo();
                JitCompileMemo::reset_compile_memo_stats();
                auto& shard = JitCompileMemo::shard_for(source.start_addr);
                const u64 before = shard.publication_sequence.load(std::memory_order_acquire);
                JitCompiler compiler(*m_memory);
                ASSERT_NE(compiler.compile(source), nullptr);
                EXPECT_EQ(JitCompileMemo::compile_memo_stats().fresh_emits, 1u);
                EXPECT_EQ(JitCompileMemo::compile_memo_stats().compile_probes, 1u);
                const u64 published = shard.publication_sequence.load(std::memory_order_acquire);
                EXPECT_EQ(published, before + 1);
                JitCompileMemo::clear_compile_memo();
                EXPECT_EQ(shard.publication_sequence.load(std::memory_order_acquire), published);
            }
        }
    }
}
