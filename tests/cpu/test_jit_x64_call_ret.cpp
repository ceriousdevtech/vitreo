// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT execution tests — x64 indirect CALL and RET

#include "jit_exec_test_fixture.hpp"

TEST_F(JitExecTest, X64RegisterIndirectCallResolvesTargetDirectlyIntoSafeHome) {
    JitLinkGuard link_guard;
    BlockCache cache;
    JitCompiler jit(*m_memory);
    jit.set_block_cache(&cache);

    // call r10. The target must survive the fault-capable guest stack store,
    // so X21 remains its across-helper home; loading it there directly avoids
    // the former X1 -> X21 copy without changing architectural ordering.
    const u8 code[] = {0x41, 0xFF, 0xD2};
    BasicBlock block = build_block64_at(0x1000, code, sizeof(code));
    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr);

    EXPECT_FALSE(code_contains_word(
        *compiled, arm64::mov_x(REG_CALL_TMP, REG_SCRATCH1)));

    m_cpu->init_long_mode();
    m_cpu->gpr.rsp = 0x10000;
    m_cpu->gpr.r10 = 0x2000;
    m_cpu->rip = 0x1000;
    ASSERT_EQ(test::execute_like_dispatcher(jit, *compiled, *m_cpu), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->rip, 0x2000u);
    EXPECT_EQ(m_cpu->gpr.rsp, 0xFFF8u);
    EXPECT_EQ(m_memory->read_u64(0xFFF8).value_or(0), 0x1003u);
}

// Same home for a memory operand: call [rbx] loads the target straight into
// X21 rather than into X1 and copying it. RED before the change (the load
// landed in X1, then MOV X21, X1).
TEST_F(JitExecTest, X64MemoryIndirectCallLoadsTargetDirectlyIntoSafeHome) {
    JitLinkGuard link_guard;
    BlockCache cache;
    JitCompiler jit(*m_memory);
    jit.set_block_cache(&cache);

    const u8 code[] = {0xFF, 0x13};  // call qword ptr [rbx]
    BasicBlock block = build_block64_at(0x1000, code, sizeof(code));
    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr);

    EXPECT_FALSE(code_contains_word(
        *compiled, arm64::mov_x(REG_CALL_TMP, REG_SCRATCH1)));

    ASSERT_TRUE(m_memory->write_u64(0x8000, 0x2000));
    m_cpu->init_long_mode();
    m_cpu->gpr.rsp = 0x10000;
    m_cpu->gpr.rbx = 0x8000;
    m_cpu->rip = 0x1000;
    ASSERT_EQ(test::execute_like_dispatcher(jit, *compiled, *m_cpu), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->rip, 0x2000u);
    EXPECT_EQ(m_cpu->gpr.rsp, 0xFFF8u);
    EXPECT_EQ(m_memory->read_u64(0xFFF8).value_or(0), 0x1002u);
}

TEST_F(JitExecTest, X64RetUsesGuestModifiedReturnAddress) {
    // Intel SDM Vol. 2 RET: the qword at [RSP] is the architectural return
    // target. A native continuation may predict 0x1005, but it must still
    // observe that the callee replaced the saved RIP with 0x3000.
    JitLinkGuard link_guard;
    BlockCache cache;
    m_jit->set_block_cache(&cache);

    auto publish64 = [&](GuestAddr address, const u8* code, usize length) {
        BasicBlock block = build_block64_at(address, code, length);
        BasicBlock* cached = cache.insert(std::move(block));
        ASSERT_NE(cached, nullptr);
        CompiledBlock* compiled = m_jit->compile(*cached);
        ASSERT_NE(compiled, nullptr);
        publish_compiled(*cached, *compiled);
        ASSERT_NE(cache.lookup(address), nullptr);
    };

    const u8 predicted_return[] = {
        0xB8, 0x11, 0x00, 0x00, 0x00, // mov eax,0x11 (must not execute)
    };
    const u8 alternate_return[] = {
        0xB8, 0x22, 0x00, 0x00, 0x00, // mov eax,0x22
    };
    const u8 callee[] = {
        0x48, 0xB8, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x48, 0x89, 0x04, 0x24,       // mov [rsp],rax
        0xC3,                         // ret
    };
    publish64(0x1005, predicted_return, sizeof(predicted_return));
    publish64(0x3000, alternate_return, sizeof(alternate_return));
    publish64(0x2000, callee, sizeof(callee));

    const u8 caller[] = {
        0xE8, 0xFB, 0x0F, 0x00, 0x00, // call 0x2000; predicted RIP 0x1005
    };
    BasicBlock caller_block = build_block64_at(0x1000, caller, sizeof(caller));
    CompiledBlock* caller_compiled = m_jit->compile(caller_block);
    ASSERT_NE(caller_compiled, nullptr);

    m_cpu->init_long_mode();
    m_cpu->gpr.rsp = 0x10000;
    m_cpu->gpr.rax = 0;
    m_cpu->rip = 0x1000;
    m_cpu->flags.rflags = 0x8D7u;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *caller_compiled, *m_cpu),
              JitExecResult::Continue);
    m_cpu->materialize_lazy_flags();

    EXPECT_EQ(m_cpu->gpr.rax, 0x22u);
    EXPECT_EQ(m_cpu->gpr.rsp, 0x10000u);
    EXPECT_EQ(m_cpu->rip, 0x3005u);
    EXPECT_EQ(m_cpu->flags.rflags, 0x8D7u);
    m_jit->set_block_cache(nullptr);
}

TEST_F(JitExecTest, X64NestedCallRetImm16RestoresGuestStack) {
    // outer pushes one argument, mid calls inner, then RET 8 releases the
    // outer argument. This exercises nested LIFO return targets plus the
    // zero-extended imm16 cleanup in one linked x64 chain.
    JitLinkGuard link_guard;
    BlockCache cache;
    m_jit->set_block_cache(&cache);

    auto publish64 = [&](GuestAddr address, const u8* code, usize length) {
        BasicBlock block = build_block64_at(address, code, length);
        BasicBlock* cached = cache.insert(std::move(block));
        ASSERT_NE(cached, nullptr);
        CompiledBlock* compiled = m_jit->compile(*cached);
        ASSERT_NE(compiled, nullptr);
        publish_compiled(*cached, *compiled);
        ASSERT_NE(cache.lookup(address), nullptr);
    };

    const u8 outer_return[] = {0x48, 0x89, 0xC2}; // mov rdx,rax
    const u8 mid_return[] = {0xC2, 0x08, 0x00};   // ret 8
    const u8 inner[] = {
        0xB8, 0x07, 0x00, 0x00, 0x00, // mov eax,7
        0xC3,                         // ret
    };
    const u8 mid_call[] = {
        0xE8, 0xFB, 0x0F, 0x00, 0x00, // call 0x3000; return 0x2005
    };
    publish64(0x1007, outer_return, sizeof(outer_return));
    publish64(0x2005, mid_return, sizeof(mid_return));
    publish64(0x3000, inner, sizeof(inner));
    publish64(0x2000, mid_call, sizeof(mid_call));

    const u8 outer[] = {
        0x6A, 0x11,                   // push 0x11
        0xE8, 0xF9, 0x0F, 0x00, 0x00, // call 0x2000; return 0x1007
    };
    BasicBlock outer_block = build_block64_at(0x1000, outer, sizeof(outer));
    CompiledBlock* outer_compiled = m_jit->compile(outer_block);
    ASSERT_NE(outer_compiled, nullptr);

    m_cpu->init_long_mode();
    m_cpu->gpr.rsp = 0x10000;
    m_cpu->gpr.rax = 0;
    m_cpu->gpr.rdx = 0;
    m_cpu->rip = 0x1000;
    m_cpu->flags.rflags = 0x8D7u;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *outer_compiled, *m_cpu),
              JitExecResult::Continue);
    m_cpu->materialize_lazy_flags();

    EXPECT_EQ(m_cpu->gpr.rax, 7u);
    EXPECT_EQ(m_cpu->gpr.rdx, 7u);
    EXPECT_EQ(m_cpu->gpr.rsp, 0x10000u);
    EXPECT_EQ(m_cpu->rip, 0x100Au);
    EXPECT_EQ(m_cpu->flags.rflags, 0x8D7u);
    m_jit->set_block_cache(nullptr);
}

TEST_F(JitExecTest, X64RetUnmappedStackReadFaultIsPrecise) {
    // RET must complete its stack read before changing RSP or RIP. An
    // unmapped [RSP] therefore raises a read AV attributed to RET with all
    // architectural state still at the instruction boundary.
    const u8 code[] = {0xC3};
    BasicBlock block = build_block64(code, sizeof(code));
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    constexpr u64 unmapped_stack = 0x20000000ull;
    m_cpu->init_long_mode();
    m_cpu->gpr.rsp = unmapped_stack;
    m_cpu->rip = 0x1000;
    m_cpu->flags.rflags = 0x8D7u;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, *m_cpu), JitExecResult::Continue);
    m_cpu->materialize_lazy_flags();

    ASSERT_TRUE(m_cpu->pending_raise.active);
    EXPECT_EQ(m_cpu->pending_raise.code, 0xC0000005u);
    EXPECT_EQ(m_cpu->pending_raise.args[0], 0u);
    EXPECT_EQ(m_cpu->pending_raise.args[1], unmapped_stack);
    EXPECT_EQ(m_cpu->rip, 0x1000u);
    EXPECT_EQ(m_cpu->gpr.rsp, unmapped_stack);
    EXPECT_EQ(m_cpu->flags.rflags, 0x8D7u);
}
