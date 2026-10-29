// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// Linear64 helper-table publication, crash breadcrumbs and the native CALL
// landing pad and its memo. Split from test_jit_linear.cpp; shared fixtures and
// matchers live in jit_linear_fixture.hpp.

#include "jit_linear_fixture.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test;

namespace {

// Execute a block compiled elsewhere after the consumer has already observed
// an earlier helper generation. Run this test alone to start with empty tables.
TEST_F(JitLinear64Test, PublishesHelpersCreatedAfterFirstExecution) {
    const std::vector<u8> nop{0x90};
    ASSERT_TRUE(m_memory->write_bytes(kCode64, nop.data(), nop.size()));
    auto* initial = m_jit->compile(build64(nop));
    ASSERT_NE(initial, nullptr);
    auto cpu = cpu64(kData64);
    ASSERT_EQ(test::execute_like_dispatcher(*m_jit, *initial, cpu), JitExecResult::Continue);

    JitCompiler producer(*m_memory);
    constexpr GuestAddr high = 0x00007FFA10000000ull;
    ASSERT_TRUE(m_memory->map_region(high, 0x4000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->write_u64(high, 0x1122334455667788ull));
    const std::vector<u8> load{0x48, 0x8B, 0x03}; // MOV RAX,[RBX]
    auto body = build64(load);
    body.start_addr += 0x200;
    ASSERT_TRUE(m_memory->write_bytes(body.start_addr, load.data(), load.size()));
    auto* shared = producer.compile(body);
    ASSERT_NE(shared, nullptr);
    for (int repeat = 0; repeat < 3; ++repeat) {
        cpu = cpu64(high);
        cpu.rip = body.start_addr;
        ASSERT_EQ(test::execute_like_dispatcher(*m_jit, *shared, cpu), JitExecResult::Continue);
        EXPECT_FALSE(cpu.pending_raise.active);
        EXPECT_EQ(cpu.gpr.get64(EAX), 0x1122334455667788ull);
    }
    ASSERT_TRUE(m_memory->protect_region(high, 0x4000, MemoryProtection::None));
    cpu = cpu64(high);
    cpu.rip = body.start_addr;
    (void)test::execute_like_dispatcher(*m_jit, *shared, cpu);
    EXPECT_TRUE(cpu.pending_raise.active);
}

TEST_F(JitLinear64Test, CrashBreadcrumbsBelongToExecutingThread) {
    const std::vector<u8> code{0x90};
    CompiledBlock* blocks[2]{};
    for (int i = 0; i < 2; ++i) {
        auto body = build64(code);
        body.start_addr += static_cast<u64>(i) * 0x200;
        ASSERT_TRUE(m_memory->write_bytes(body.start_addr, code.data(), code.size()));
        blocks[i] = m_jit->compile(body);
        ASSERT_NE(blocks[i], nullptr);
    }
    const u64 caller_record = jit_crash_breadcrumbs.last_block_addr;
    std::atomic<int> finished{0};
    bool pristine[2]{};
    u64 last_block[2]{};
    u32 last_sp[2]{};
    JitExecResult results[2]{};
    std::vector<std::thread> threads;
    for (int i = 0; i < 2; ++i) {
        threads.emplace_back([&, i] {
            pristine[i] = jit_crash_breadcrumbs.last_block_addr == 0;
            JitCompiler consumer(*m_memory);
            auto cpu = cpu64(kData64);
            cpu.rip = blocks[i]->guest_addr;
            cpu.gpr.set64(ESP, kData64 + static_cast<u64>(i) * 0x100);
            results[i] = consumer.execute(*blocks[i], cpu);
            finished.fetch_add(1, std::memory_order_release);
            while (finished.load(std::memory_order_acquire) != 2)
                std::this_thread::yield();
            last_block[i] = jit_crash_breadcrumbs.last_block_addr;
            last_sp[i] = jit_crash_breadcrumbs.guest_esp;
        });
    }
    for (auto& thread : threads) thread.join();
    for (int i = 0; i < 2; ++i) {
        EXPECT_TRUE(pristine[i]);
        EXPECT_EQ(results[i], JitExecResult::Continue);
        EXPECT_EQ(last_block[i], blocks[i]->guest_addr);
        EXPECT_EQ(last_sp[i], static_cast<u32>(kData64 + i * 0x100));
    }
    EXPECT_EQ(jit_crash_breadcrumbs.last_block_addr, caller_record);
}

TEST_F(JitLinear64Test, DISABLED_HelperPublicationBenchmark) {
    const std::vector<u8> code{0x90};
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    auto* block = m_jit->compile(build64(code));
    ASSERT_NE(block, nullptr);
    constexpr int threads = 12;
    constexpr int iterations = 2000000;
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::atomic<int> invalid_workers{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < threads; ++i) {
        workers.emplace_back([&] {
            JitCompiler consumer(*m_memory);
            auto cpu = cpu64(kData64);
            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            bool valid = true;
            for (int n = 0; n < iterations; ++n) {
                cpu.rip = kCode64;
                const auto result = consumer.execute(*block, cpu);
                valid &= result == JitExecResult::Continue &&
                         cpu.rip == kCode64 + code.size();
            }
            if (!valid) invalid_workers.fetch_add(1);
        });
    }
    while (ready.load() != threads) std::this_thread::yield();
    const auto start = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    EXPECT_EQ(invalid_workers.load(), 0);
    std::printf("HELPER_BENCH seconds=%.6f executions=%d ns_per_execution=%.3f\n",
                seconds, threads * iterations,
                seconds * 1e9 / (threads * iterations));
}

TEST_F(JitLinear64Test, LoadedAccumulatorSurvivesLockedCompareExchange) {
    // Intel SDM CMPXCHG: a successful exchange preserves the old EAX.
    // Include the linear memory load and a separate successor TEST block.
    const std::vector<u8> code{
        0x41, 0x8B, 0x06,                    // mov eax,[r14]
        0x89, 0xC1, 0x83, 0xE1, 0xFE,       // mov ecx,eax; and ecx,-2
        0xF0, 0x41, 0x0F, 0xB1, 0x0E,       // lock cmpxchg [r14],ecx
        0x75, 0xF4};                         // jne retry at mov ecx,eax
    const std::vector<u8> tail{0xA8, 0x01, 0x74, 0x02};
    auto block = build64(code);
    auto successor = build64(tail);
    successor.start_addr = kCode64 + code.size();
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()).is_ok());
    ASSERT_TRUE(m_memory->write_bytes(successor.start_addr, tail.data(), tail.size()).is_ok());
    auto* compiled = m_jit->compile(block);
    auto* next = m_jit->compile(successor);
    ASSERT_NE(compiled, nullptr);
    ASSERT_NE(next, nullptr);
    for (const u32 initial : {1u, 3u, 0x80000001u}) {
        SCOPED_TRACE(initial);
        ASSERT_TRUE(m_memory->write_u32(kData64, initial).is_ok());
        auto state = cpu64(kData64);
        state.gpr.set64(14, kData64);
        test::execute_like_dispatcher(*m_jit, *compiled, state);
        ASSERT_EQ(state.rip, successor.start_addr);
        EXPECT_EQ(state.gpr.get64(EAX), initial);
        EXPECT_EQ(m_memory->read_u32(kData64).value(), initial & ~1u);
        test::execute_like_dispatcher(*m_jit, *next, state);
        state.materialize_lazy_flags();
        EXPECT_FALSE(state.flags.zf());
        EXPECT_EQ(state.rip, successor.start_addr + tail.size());
    }
}

// Guest flags across a linear64 STORE, on both the inline path and the helper.
//
// `emit_linear64_store` contains no instruction that writes NZCV -- its guard
// is ORR/LSR/CBNZ plus an LDRB/CBNZ check byte and the store is STR/STLR -- so
// `linear64_store_is_nzcv_clean()` lets the inline MRS/STR + LDR/MSR pair go
// and `ColdMemStub::spill_nzcv` pays it only when the helper actually runs.
// Getting that wrong is a SILENT wrong-answer bug: a branch after a store
// simply reads the wrong flags.
//
// Deliberately in this fixture and not in JitX64InlineMemTest. That one never
// enables a linear reservation, so all of its stores take
// `emit_x64_region_store` -- the path this optimization intentionally does NOT
// touch -- and the same test there passes with the stub's restore deleted.
// It also compares GPRs and memory but not flags, so SETcc is what makes the
// flags observable at all.
TEST_F(JitLinear64Test, FlagsSurviveLinear64Store) {
    // cmp rbx,rcx ; mov [rax],edx ; setb r8b ; sete r9b
    const std::vector<u8> code = {
        0x48, 0x39, 0xCB,
        0x89, 0x10,
        0x41, 0x0F, 0x92, 0xC0,
        0x41, 0x0F, 0x94, 0xC1,
    };
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    // Run the SAME store twice: once on an unmarked page, where the inline
    // NZCV-clean path is used, and once with a live MEM_WRITE_WATCH
    // allocation over it, which is what forces a linear64 store onto the
    // checked helper (ColdMemStub::watch_branch_pos).
    //
    // Only the second arm has teeth -- the inline path preserves NZCV by
    // construction and so cannot detect a stub that forgets to restore.
    // Verified by deleting the stub's MRS/STR and watching this go red.
    //
    // The two arms use the same ADDRESS rather than two addresses on one
    // page: kData64 maps exactly one 16 KiB host page, and the check byte has
    // host-page granularity, so any second address here would be marked too.
    const GuestAddr dest = kData64 + 0x40;
    for (const bool watched : {false, true}) {
        if (watched) m_memory->linear_watch_range_retain(kData64, 0x4000);
        for (const auto& pair : {std::pair<u64, u64>{1, 2},
                                 std::pair<u64, u64>{2, 2},
                                 std::pair<u64, u64>{3, 2}}) {
            const auto seed = [&](CpuState& c) {
                c.init_long_mode();
                c.rip = kCode64;
                c.gpr.set64(0, dest);          // RAX
                c.gpr.set64(3, pair.first);    // RBX
                c.gpr.set64(1, pair.second);   // RCX
                c.gpr.set64(2, 0xA5A5A5A5u);   // RDX
            };

            CpuState oracle;
            seed(oracle);
            {
                Interpreter interp(oracle, *m_memory);
                for (u32 i = 0; i < block.instruction_count; ++i) {
                    ASSERT_EQ(interp.step(), ExecResult::Continue);
                }
            }
            oracle.materialize_lazy_flags();

            CpuState jit_cpu;
            seed(jit_cpu);
            (void)test::execute_like_dispatcher(*m_jit, *compiled, jit_cpu);
            jit_cpu.materialize_lazy_flags();

            EXPECT_FALSE(jit_cpu.pending_raise.active);
            // R8 = below, R9 = equal: a lost CF and a lost ZF are both caught.
            EXPECT_EQ(jit_cpu.gpr.get64(8), oracle.gpr.get64(8))
                << "CF lost across a store (watched=" << watched << ", "
                << pair.first << " vs " << pair.second << ")";
            EXPECT_EQ(jit_cpu.gpr.get64(9), oracle.gpr.get64(9))
                << "ZF lost across a store (watched=" << watched << ")";
            EXPECT_EQ(jit_cpu.flags.rflags, oracle.flags.rflags)
                << "EFLAGS diverged, watched=" << watched;
        }
        if (watched) m_memory->linear_watch_range_release(kData64, 0x4000);
    }
}

TEST_F(JitLinear64Test, NativeCallLandingPadPushesOneFrameThenJoinsLinkEntry) {
    ScopedEnvironmentFlag native_callret(
        "VITREO_JIT_NATIVE_CALLRET", "1");
    const std::vector<u8> code = {
        0xB8, 0x01, 0x00, 0x00, 0x00, // mov eax,1
    };
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    JitCompiler enabled_jit(*m_memory);
    enabled_jit.set_block_cache(m_cache.get());
    CompiledBlock* compiled = enabled_jit.compile(block);
    ASSERT_NE(compiled, nullptr);
    ASSERT_FALSE(compiled->requires_protection_lease);
    ASSERT_NE(compiled->link_entry(), nullptr);
    ASSERT_NE(compiled->call_entry(), nullptr);

    const auto* call = static_cast<const u8*>(compiled->call_entry());
    const auto* words = reinterpret_cast<const u32*>(call);
    // The dispatcher prologue branches over the pad, which pushes no frame
    // for an entry the dispatcher made.
    EXPECT_EQ(words[-1], arm64::b_imm(6 * sizeof(u32)));
    EXPECT_EQ(words[0], arm64::ldr_x_uoff(3, REG_JIT_CTX,
                                          JIT_CTX_OFF_NATIVE_SP_FLOOR >> 3));
    EXPECT_EQ(words[1], arm64::sub_x_sp_reg(3, 3));
    EXPECT_EQ(words[2], arm64::tbnz(3, 63, 3 * sizeof(u32)))
        << "below the floor the pad must enter without a frame";
    EXPECT_EQ(words[3], 0xA9BE7FFEu)  // STP X30,XZR,[SP,#-32]!
        << "the frame keeps [SP+8] free for the block NZCV spill slot";
    EXPECT_EQ(words[4], arm64::stp_x_off(
                            REG_CALL_TMP, REG_JIT_CTX, 31,
                            kNativeCallGuestRipOffset / sizeof(u64)))
        << "guest return RIP and the JitContext tag, for the RET check";
    EXPECT_EQ(call + 5 * sizeof(u32), compiled->link_entry())
        << "the pad falls straight into the linked entry";
}

// A hardware fault two native calls deep: the continuation frames sit between
// SP and the dispatcher LR frame, and only the fault handler's restore of
// JitContext::native_sp_base lets the shared trampoline pop the right frame.
// Getting it wrong returns into translated code or crashes the host.
TEST_F(JitLinear64Test, FaultUnderNativeCallFramesReturnsToTheDispatcher) {
    ScopedEnvironmentFlag native_callret("VITREO_JIT_NATIVE_CALLRET", "1");
    struct LinkOn {
        LinkOn() { JitCompiler::enable_jit_link(); }
        ~LinkOn() { JitCompiler::disable_jit_link(); }
    } link_on;
    if (!JitCompiler::link_edge_enabled(JitCompiler::LinkEdge::CallRel)) {
        GTEST_SKIP() << "CALL rel direct links are disabled";
    }
    JitCompiler jit(*m_memory);
    jit.set_block_cache(m_cache.get());

    auto publish = [&](GuestAddr address, std::vector<u8> code) {
        EXPECT_TRUE(m_memory->write_bytes(address, code.data(), code.size()));
        BasicBlock block = build64(code);
        block.start_addr = address;
        BasicBlock* cached = m_cache->insert(std::move(block));
        EXPECT_NE(cached, nullptr);
        CompiledBlock* compiled = jit.compile(*cached);
        EXPECT_NE(compiled, nullptr);
        jit.publish_compiled_block(*cached, *compiled);
        return compiled;
    };
    // +0x200: mov rax,[rbx] (rbx unmapped) ; ret
    publish(kCode64 + 0x200, {0x48, 0x8B, 0x03, 0xC3});
    publish(kCode64 + 0x105, {0xC3});                          // ret
    publish(kCode64 + 0x100, {0xE8, 0xFB, 0x00, 0x00, 0x00});  // call +0x200
    publish(kCode64 + 0x005, {0x48, 0x89, 0xC2});              // mov rdx,rax
    CompiledBlock* entry =
        publish(kCode64, {0xE8, 0xFB, 0x00, 0x00, 0x00});      // call +0x100

    for (int round = 0; round < 3; ++round) {
        SCOPED_TRACE(round);
        CpuState cpu = cpu64(kUnmapped64);
        cpu.gpr.rsp = kData64 + 0x2000;
        EXPECT_EQ(test::execute_like_dispatcher(jit, *entry, cpu), JitExecResult::Continue);
        ASSERT_TRUE(cpu.pending_raise.active);
        EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
        EXPECT_EQ(cpu.pending_raise.args[1], kUnmapped64);
        EXPECT_EQ(cpu.rip, kCode64 + 0x200);
        EXPECT_EQ(cpu.gpr.rsp, kData64 + 0x2000 - 16);
        EXPECT_EQ(m_memory->read_u64(kData64 + 0x2000 - 8).value_or(0),
                  kCode64 + 0x005);
        EXPECT_EQ(m_memory->read_u64(kData64 + 0x2000 - 16).value_or(0),
                  kCode64 + 0x105);
    }
}

TEST_F(JitLinear64Test, NativeCallLandingPadDefaultsOffAndMemoSeparatesGate) {
    ScopedCompileMemoIsolation memo_isolation;
    ScopedEnvironmentFlag native_callret(
        "VITREO_JIT_NATIVE_CALLRET", nullptr);
    const std::vector<u8> code = {
        0x48, 0x01, 0xC8, // add rax,rcx
    };
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    JitCompileMemo::enable_compile_memo();

    JitCompiler disabled_jit(*m_memory);
    CompiledBlock* disabled = disabled_jit.compile(block);
    ASSERT_NE(disabled, nullptr);
    EXPECT_EQ(disabled->call_code_offset, disabled->code_size);
    EXPECT_EQ(disabled->call_entry(), nullptr);
    void* disabled_code = disabled->code;

    ScopedEnvironmentFlag enabled_flag(
        "VITREO_JIT_NATIVE_CALLRET", "1");
    JitCompiler enabled_jit(*m_memory);
    CompiledBlock* enabled = enabled_jit.compile(block);
    ASSERT_NE(enabled, nullptr);
    EXPECT_NE(enabled->code, disabled_code)
        << "native CALL/RET feature state shared a compile-memo entry";
    EXPECT_NE(enabled->call_entry(), nullptr);
}

TEST_F(JitLinear64Test, NativeCallEntryMemoRoundTripPreservesPublishedOffset) {
    ScopedCompileMemoIsolation memo_isolation;
    ScopedEnvironmentFlag native_callret(
        "VITREO_JIT_NATIVE_CALLRET", "1");
    const std::vector<u8> code = {
        0x48, 0x01, 0xC8, // add rax,rcx
    };
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    ASSERT_EQ(block.instruction_count, 1u);
    JitCompileMemo::enable_compile_memo();

    JitCompiler publisher(*m_memory);
    CompiledBlock* first = publisher.compile(block);
    ASSERT_NE(first, nullptr);
    ASSERT_LT(first->call_code_offset, first->code_size);
    ASSERT_NE(first->call_entry(), nullptr);
    void* memo_code = first->code;
    const u32 memo_call_offset = first->call_code_offset;

    JitCompiler consumer(*m_memory);
    CompiledBlock* reused = consumer.compile(block);
    ASSERT_NE(reused, nullptr);
    EXPECT_EQ(reused->code, memo_code)
        << "second compiler did not exercise the shared memo record";
    EXPECT_EQ(reused->call_code_offset, memo_call_offset);
    EXPECT_EQ(reused->call_entry(),
              static_cast<u8*>(memo_code) + memo_call_offset)
        << "memo reuse lost the published native CALL landing pad";
}

}  // namespace
