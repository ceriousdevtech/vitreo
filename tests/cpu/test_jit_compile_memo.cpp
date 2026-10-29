// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT execution tests — compile memo and per-compile state isolation

#include "jit_exec_test_fixture.hpp"

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
    // same instant legitimately each emit a copy — the memo dedupes the
    // translation, it does not serialise first compiles behind a per-address
    // lock, which is exactly the process-wide mutex this design avoids.
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
