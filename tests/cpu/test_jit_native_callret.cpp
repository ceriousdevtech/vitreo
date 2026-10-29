// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT execution tests — native CALL/RET protocol

#include "jit_exec_test_fixture.hpp"

namespace {

class NativeCallretFlagGuard {
public:
    explicit NativeCallretFlagGuard(const char* value) {
        remember("VITREO_JIT_NATIVE_CALLRET", m_native_previous,
                 m_had_native);
        if (value != nullptr) setenv("VITREO_JIT_NATIVE_CALLRET", value, 1);
        else unsetenv("VITREO_JIT_NATIVE_CALLRET");
    }

    ~NativeCallretFlagGuard() {
        restore("VITREO_JIT_NATIVE_CALLRET", m_native_previous,
                m_had_native);
    }

private:
    static void remember(const char* name, std::string& value, bool& had) {
        if (const char* old = std::getenv(name)) {
            value = old;
            had = true;
        }
    }

    static void restore(const char* name, const std::string& value, bool had) {
        if (had) setenv(name, value.c_str(), 1);
        else unsetenv(name);
    }

    std::string m_native_previous;
    bool m_had_native{false};
};

usize find_word_after(const CompiledBlock& block, u32 word, usize begin = 0) {
    const auto* words = static_cast<const u32*>(block.code);
    const usize count = block.code_size / sizeof(u32);
    for (usize i = begin; i < count; ++i) {
        if (words[i] == word) return i;
    }
    return count;
}

} // namespace

TEST_F(JitExecTest, NativeDirectCallProbeGateOffKeepsCurrentLinkedLowering) {
    NativeCallretFlagGuard native_callret(nullptr);
    JitLinkGuard link_guard;
    BlockCache cache;
    JitCompiler jit(*m_memory);
    jit.set_block_cache(&cache);

    const u8 code[] = {0xE8, 0xFB, 0x0F, 0x00, 0x00}; // call 0x2000
    BasicBlock block = build_block64_at(0x1000, code, sizeof(code));
    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr);

    constexpr u32 call_off =
        static_cast<u32>(offsetof(BasicBlock, compiled_call_code));
    static_assert((call_off & 7) == 0);
    const u32 call_entry_load = arm64::ldr_x_uoff(16, 17, call_off >> 3);
    EXPECT_FALSE(code_contains_word(*compiled, call_entry_load));
    EXPECT_EQ(compiled->call_entry(), nullptr);
    // Tier 1 commits first and undoes on a fault (rsp_undo) instead.
    const u32 commit = jit.ir_emit_active()
        ? arm64::sub_x_imm(REG_PIN_ESP, REG_PIN_ESP, 8)
        : arm64::mov_x(REG_PIN_ESP, REG_CALL_TMP);
    EXPECT_TRUE(code_contains_word(*compiled, commit))
        << "gate-off CALL must retain the fault-ordered guest RSP commit";
}

TEST_F(JitExecTest, NativeDirectCallSlotFollowsTheGuestStackCommit) {
    NativeCallretFlagGuard native_callret("1");
    JitLinkGuard link_guard;
    BlockCache cache;
    JitCompiler jit(*m_memory);
    jit.set_block_cache(&cache);
    if (!JitCompiler::link_edge_enabled(JitCompiler::LinkEdge::CallRel)) {
        GTEST_SKIP() << "CALL rel direct links are disabled";
    }

    const u8 caller_code[] = {
        0xE8, 0xFB, 0x0F, 0x00, 0x00, // call 0x2000
    };
    BasicBlock caller = build_block64_at(
        0x1000, caller_code, sizeof(caller_code));
    CompiledBlock* compiled = jit.compile(caller);
    ASSERT_NE(compiled, nullptr);

    const auto* words = static_cast<const u32*>(compiled->code);
    const usize word_count = compiled->code_size / sizeof(u32);
    const usize commit = find_word_after(
        *compiled, arm64::mov_x(REG_PIN_ESP, REG_CALL_TMP));
    usize bl = word_count;
    for (usize i = 0; i < word_count; ++i) {
        if ((words[i] & 0xFC000000u) == 0x94000000u) { bl = i; break; }
    }
    ASSERT_LT(commit, word_count);
    ASSERT_LT(bl, word_count) << "native CALL must emit a patchable BL slot";
    EXPECT_LT(commit, bl)
        << "the guest CALL must retire before entering the callee natively";
}

namespace {

/// Index of the first BL in `block` whose target is `entry`, or the word count.
usize find_bl_to(const CompiledBlock& block, const void* entry) {
    const auto* words = static_cast<const u32*>(block.code);
    const usize count = block.code_size / sizeof(u32);
    for (usize i = 0; i < count; ++i) {
        if ((words[i] & 0xFC000000u) != 0x94000000u) continue;
        const i32 disp = static_cast<i32>(words[i] << 6) >> 6;
        if (reinterpret_cast<const u8*>(&words[i]) +
                static_cast<isize>(disp) * 4 ==
            static_cast<const u8*>(entry)) {
            return i;
        }
    }
    return count;
}

}  // namespace

// The completed protocol: the caller's BL is patched to the callee's landing
// pad, the callee's RET carries the native frame check, and the guest-visible
// result is exactly the ordinary CALL/RET's.
TEST_F(JitExecTest, NativeCallRetPatchesBlAndRoundTrips) {
    NativeCallretFlagGuard native_callret("1");
    JitLinkGuard link_guard;
    BlockCache cache;
    JitCompiler jit(*m_memory);
    jit.set_block_cache(&cache);
    if (!JitCompiler::link_edge_enabled(JitCompiler::LinkEdge::CallRel)) {
        GTEST_SKIP() << "CALL rel direct links are disabled";
    }

    struct Published { BasicBlock* block; CompiledBlock* compiled; };
    auto publish64 = [&](GuestAddr address, const u8* code, usize length) {
        BasicBlock block = build_block64_at(address, code, length);
        BasicBlock* cached = cache.insert(std::move(block));
        EXPECT_NE(cached, nullptr);
        CompiledBlock* compiled = jit.compile(*cached);
        EXPECT_NE(compiled, nullptr);
        jit.publish_compiled_block(*cached, *compiled);
        EXPECT_NE(cache.lookup(address), nullptr);
        return Published{cached, compiled};
    };

    const u8 continuation[] = {0x48, 0x89, 0xC2}; // mov rdx,rax
    const u8 callee[] = {
        0xB8, 0x2A, 0x00, 0x00, 0x00, // mov eax,42
        0xC3,                         // ret
    };
    const u8 caller[] = {
        0xE8, 0xFB, 0x0F, 0x00, 0x00, // call 0x2000; returns to 0x1005
    };
    publish64(0x1005, continuation, sizeof(continuation));
    const Published target = publish64(0x2000, callee, sizeof(callee));
    const Published source = publish64(0x1000, caller, sizeof(caller));
    ASSERT_NE(target.block->compiled_call_code, nullptr);
    // CompiledBlock pointers returned by compile() do not survive later
    // compiles; the cache entries are the stable view of published code.
    const CompiledBlock callee_code = compiled_from_cache(*target.block);
    CompiledBlock caller_code = compiled_from_cache(*source.block);
    EXPECT_LT(find_bl_to(caller_code, target.block->compiled_call_code),
              caller_code.code_size / sizeof(u32))
        << "the CALL site was not patched to the callee's landing pad";
    EXPECT_LT(find_word_after(callee_code,
                              arm64::ldp_x_off(3, 4, 31,
                                               kNativeCallGuestRipOffset / 8)),
              callee_code.code_size / sizeof(u32))
        << "the callee's RET carries no native frame check";

    for (int round = 0; round < 3; ++round) {
        SCOPED_TRACE(round);
        m_cpu->init_long_mode();
        m_cpu->gpr.rsp = 0x10000;
        m_cpu->gpr.rax = 0;
        m_cpu->gpr.rdx = 0;
        m_cpu->rip = 0x1000;
        m_cpu->flags.rflags = 0x8D7u;
        EXPECT_EQ(test::execute_like_dispatcher(jit, caller_code, *m_cpu), JitExecResult::Continue);
        m_cpu->materialize_lazy_flags();
        EXPECT_EQ(m_cpu->gpr.rax, 42u);
        EXPECT_EQ(m_cpu->gpr.rdx, 42u);
        EXPECT_EQ(m_cpu->gpr.rsp, 0x10000u);
        EXPECT_EQ(m_cpu->rip, 0x1008u);
        EXPECT_EQ(m_cpu->flags.rflags, 0x8D7u);
        EXPECT_EQ(m_memory->read_u64(0xFFF8).value_or(0), 0x1005u);
    }
}

// A RET with no continuation frame -- the bottom of a chain, whose top frame
// is execute()'s {0, 0} pair -- must take the L1 tail, never return into
// the dispatcher.
TEST_F(JitExecTest, NativeRetAtChainBottomTakesTheL1Tail) {
    NativeCallretFlagGuard native_callret("1");
    JitLinkGuard link_guard;
    BlockCache cache;
    JitCompiler jit(*m_memory);
    jit.set_block_cache(&cache);

    const u8 target_code[] = {0xB8, 0x22, 0x00, 0x00, 0x00}; // mov eax,0x22
    BasicBlock target = build_block64_at(0x3000, target_code,
                                         sizeof(target_code));
    BasicBlock* cached = cache.insert(std::move(target));
    ASSERT_NE(cached, nullptr);
    CompiledBlock* target_compiled = jit.compile(*cached);
    ASSERT_NE(target_compiled, nullptr);
    publish_compiled(*cached, *target_compiled);
    ASSERT_NE(cache.lookup(0x3000), nullptr);

    const u8 ret_code[] = {0xC3};
    BasicBlock ret_block = build_block64_at(0x1000, ret_code, sizeof(ret_code));
    CompiledBlock* compiled = jit.compile(ret_block);
    ASSERT_NE(compiled, nullptr);

    ASSERT_TRUE(m_memory->write_u64(0xFFF0, 0x3000));
    m_cpu->init_long_mode();
    m_cpu->gpr.rsp = 0xFFF0;
    m_cpu->gpr.rax = 0;
    m_cpu->rip = 0x1000;
    EXPECT_EQ(test::execute_like_dispatcher(jit, *compiled, *m_cpu), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.rax, 0x22u);
    EXPECT_EQ(m_cpu->gpr.rsp, 0xFFF8u);
    EXPECT_EQ(m_cpu->rip, 0x3005u);
}

// Recursion deeper than kNativeCallMaxFrames: past native_sp_floor the pad
// pushes nothing, the matching RETs find an older frame or none, and every
// return still reaches its architectural target.
TEST_F(JitExecTest, NativeCallRetRecursionPastTheFrameCap) {
    NativeCallretFlagGuard native_callret("1");
    JitLinkGuard link_guard;
    BlockCache cache;
    JitCompiler jit(*m_memory);
    jit.set_block_cache(&cache);

    auto publish64 = [&](GuestAddr address, const u8* code, usize length) {
        BasicBlock block = build_block64_at(address, code, length);
        BasicBlock* cached = cache.insert(std::move(block));
        EXPECT_NE(cached, nullptr);
        CompiledBlock* compiled = jit.compile(*cached);
        EXPECT_NE(compiled, nullptr);
        jit.publish_compiled_block(*cached, *compiled);
        EXPECT_NE(cache.lookup(address), nullptr);
        return compiled;
    };

    // f: dec ecx ; jz .done ; call f ; .done: add eax,1 ; ret
    const u8 f_head[] = {
        0xFF, 0xC9,                   // 0x2000 dec ecx
        0x74, 0x05,                   // 0x2002 jz 0x2009
        0xE8, 0xF7, 0xFF, 0xFF, 0xFF, // 0x2004 call 0x2000
    };
    const u8 f_tail[] = {
        0x83, 0xC0, 0x01,             // 0x2009 add eax,1
        0xC3,                         // 0x200C ret
    };
    const u8 continuation[] = {0x48, 0x89, 0xC2}; // 0x1005 mov rdx,rax
    publish64(0x1005, continuation, sizeof(continuation));
    publish64(0x2009, f_tail, sizeof(f_tail));
    publish64(0x2000, f_head, sizeof(f_head));
    const u8 call_f[] = {0xE8, 0xFB, 0x0F, 0x00, 0x00}; // call 0x2000
    CompiledBlock* source = publish64(0x1000, call_f, sizeof(call_f));
    ASSERT_NE(source, nullptr);

    constexpr u32 kDepth = 3 * kNativeCallMaxFrames / 2;
    m_cpu->init_long_mode();
    m_cpu->gpr.rsp = 0x10000;
    m_cpu->gpr.rax = 0;
    m_cpu->gpr.rdx = 0;
    m_cpu->gpr.rcx = kDepth;
    m_cpu->rip = 0x1000;
    // Hop budgets may return to the dispatcher mid-recursion; resume from
    // the cache exactly as the dispatcher would.
    for (int entries = 0; entries < 64 && m_cpu->rip != 0x1008; ++entries) {
        BasicBlock* block = cache.lookup(m_cpu->rip);
        ASSERT_NE(block, nullptr) << std::hex << m_cpu->rip;
        CompiledBlock entry = compiled_from_cache(*block);
        ASSERT_EQ(test::execute_like_dispatcher(jit, entry, *m_cpu), JitExecResult::Continue);
    }
    EXPECT_EQ(m_cpu->rip, 0x1008u);
    EXPECT_EQ(m_cpu->gpr.rcx, 0u);
    EXPECT_EQ(m_cpu->gpr.rax, kDepth);
    EXPECT_EQ(m_cpu->gpr.rdx, kDepth);
    EXPECT_EQ(m_cpu->gpr.rsp, 0x10000u);
}

// Intel SDM Vol. 2 RET: the qword at [RSP] is the target, whatever a native
// continuation frame predicts.
TEST_F(JitExecTest, NativeRetUsesGuestModifiedReturnAddress) {
    NativeCallretFlagGuard native_callret("1");
    JitLinkGuard link_guard;
    BlockCache cache;
    JitCompiler jit(*m_memory);
    jit.set_block_cache(&cache);

    auto publish64 = [&](GuestAddr address, const u8* code, usize length) {
        BasicBlock block = build_block64_at(address, code, length);
        BasicBlock* cached = cache.insert(std::move(block));
        EXPECT_NE(cached, nullptr);
        CompiledBlock* compiled = jit.compile(*cached);
        EXPECT_NE(compiled, nullptr);
        jit.publish_compiled_block(*cached, *compiled);
        return compiled;
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
    const u8 caller[] = {
        0xE8, 0xFB, 0x0F, 0x00, 0x00, // call 0x2000; predicted RIP 0x1005
    };
    publish64(0x1005, predicted_return, sizeof(predicted_return));
    publish64(0x3000, alternate_return, sizeof(alternate_return));
    publish64(0x2000, callee, sizeof(callee));
    CompiledBlock* source = publish64(0x1000, caller, sizeof(caller));
    ASSERT_NE(source, nullptr);

    for (int round = 0; round < 2; ++round) {
        SCOPED_TRACE(round);
        m_cpu->init_long_mode();
        m_cpu->gpr.rsp = 0x10000;
        m_cpu->gpr.rax = 0;
        m_cpu->rip = 0x1000;
        m_cpu->flags.rflags = 0x8D7u;
        EXPECT_EQ(test::execute_like_dispatcher(jit, *source, *m_cpu), JitExecResult::Continue);
        m_cpu->materialize_lazy_flags();
        EXPECT_EQ(m_cpu->gpr.rax, 0x22u);
        EXPECT_EQ(m_cpu->gpr.rsp, 0x10000u);
        EXPECT_EQ(m_cpu->rip, 0x3005u);
        EXPECT_EQ(m_cpu->flags.rflags, 0x8D7u);
    }
}

// Nested native calls with RET imm16: LIFO frames, and the imm16 cleanup
// still applies to the native return.
TEST_F(JitExecTest, NativeNestedCallRetImm16RestoresGuestStack) {
    NativeCallretFlagGuard native_callret("1");
    JitLinkGuard link_guard;
    BlockCache cache;
    JitCompiler jit(*m_memory);
    jit.set_block_cache(&cache);

    auto publish64 = [&](GuestAddr address, const u8* code, usize length) {
        BasicBlock block = build_block64_at(address, code, length);
        BasicBlock* cached = cache.insert(std::move(block));
        EXPECT_NE(cached, nullptr);
        CompiledBlock* compiled = jit.compile(*cached);
        EXPECT_NE(compiled, nullptr);
        jit.publish_compiled_block(*cached, *compiled);
        return compiled;
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
    const u8 outer[] = {
        0x6A, 0x11,                   // push 0x11
        0xE8, 0xF9, 0x0F, 0x00, 0x00, // call 0x2000; return 0x1007
    };
    publish64(0x1007, outer_return, sizeof(outer_return));
    publish64(0x2005, mid_return, sizeof(mid_return));
    publish64(0x3000, inner, sizeof(inner));
    publish64(0x2000, mid_call, sizeof(mid_call));
    CompiledBlock* source = publish64(0x1000, outer, sizeof(outer));
    ASSERT_NE(source, nullptr);

    m_cpu->init_long_mode();
    m_cpu->gpr.rsp = 0x10000;
    m_cpu->gpr.rax = 0;
    m_cpu->gpr.rdx = 0;
    m_cpu->rip = 0x1000;
    m_cpu->flags.rflags = 0x8D7u;
    EXPECT_EQ(test::execute_like_dispatcher(jit, *source, *m_cpu), JitExecResult::Continue);
    m_cpu->materialize_lazy_flags();
    EXPECT_EQ(m_cpu->gpr.rax, 7u);
    EXPECT_EQ(m_cpu->gpr.rdx, 7u);
    EXPECT_EQ(m_cpu->gpr.rsp, 0x10000u);
    EXPECT_EQ(m_cpu->rip, 0x100Au);
    EXPECT_EQ(m_cpu->flags.rflags, 0x8D7u);
}

TEST_F(JitExecTest, NativeDirectCallMissPreservesLiveArithmeticFlags) {
    NativeCallretFlagGuard native_callret("1");
    JitLinkGuard link_guard;
    BlockCache cache;
    JitCompiler jit(*m_memory);
    jit.set_block_cache(&cache);

    const u8 caller_code[] = {
        0x48, 0x39, 0xC0,              // cmp rax,rax
        0xE8, 0xF8, 0x0F, 0x00, 0x00, // call 0x2000
    };
    BasicBlock caller = build_block64_at(
        0x1000, caller_code, sizeof(caller_code));
    CompiledBlock* compiled = jit.compile(caller);
    ASSERT_NE(compiled, nullptr);

    CpuState reference;
    reference.init_long_mode();
    reference.gpr.rax = 0x1234;
    reference.gpr.rsp = 0x10000;
    reference.rip = 0x1000;
    reference.flags.rflags = 0x8D7u;
    Interpreter interpreter(reference, *m_memory);
    ASSERT_EQ(interpreter.run(2), ExecResult::Branch);
    reference.materialize_lazy_flags();

    m_cpu->init_long_mode();
    m_cpu->gpr.rax = 0x1234;
    m_cpu->gpr.rsp = 0x10000;
    m_cpu->rip = 0x1000;
    m_cpu->flags.rflags = 0x8D7u;
    ASSERT_EQ(test::execute_like_dispatcher(jit, *compiled, *m_cpu), JitExecResult::Continue);
    m_cpu->materialize_lazy_flags();

    EXPECT_EQ(m_cpu->rip, reference.rip);
    EXPECT_EQ(m_cpu->gpr.rsp, reference.gpr.rsp);
    EXPECT_EQ(m_cpu->flags.rflags, reference.flags.rflags);
}

TEST_F(JitExecTest, NativeDirectCallProbeFallbacksPreserveGuestContract) {
    NativeCallretFlagGuard native_callret("1");
    JitLinkGuard link_guard;
    BlockCache cache;
    JitCompiler jit(*m_memory);
    jit.set_block_cache(&cache);

    auto publish64 = [&](GuestAddr address, const u8* code, usize length) {
        BasicBlock block = build_block64_at(address, code, length);
        BasicBlock* cached = cache.insert(std::move(block));
        EXPECT_NE(cached, nullptr);
        if (cached == nullptr) return static_cast<BasicBlock*>(nullptr);
        CompiledBlock* compiled = jit.compile(*cached);
        EXPECT_NE(compiled, nullptr);
        if (compiled == nullptr) return static_cast<BasicBlock*>(nullptr);
        publish_compiled(*cached, *compiled);
        return cached;
    };

    const u8 continuation[] = {0x48, 0x89, 0xC2}; // mov rdx,rax
    const u8 callee[] = {
        0xB8, 0x2A, 0x00, 0x00, 0x00, // mov eax,42
        0xC3,                         // ret
    };
    ASSERT_NE(publish64(0x1005, continuation, sizeof(continuation)), nullptr);
    BasicBlock* target = publish64(0x2000, callee, sizeof(callee));
    ASSERT_NE(target, nullptr);

    const u8 caller_code[] = {
        0xE8, 0xFB, 0x0F, 0x00, 0x00, // call 0x2000
    };
    BasicBlock caller = build_block64_at(
        0x1000, caller_code, sizeof(caller_code));
    CompiledBlock* caller_compiled = jit.compile(caller);
    ASSERT_NE(caller_compiled, nullptr);

    const auto run = [&] {
        m_cpu->init_long_mode();
        m_cpu->gpr.rsp = 0x10000;
        m_cpu->gpr.rax = 0;
        m_cpu->gpr.rdx = 0;
        m_cpu->rip = 0x1000;
        m_cpu->flags.rflags = 0x8D7u;
        EXPECT_EQ(test::execute_like_dispatcher(jit, *caller_compiled, *m_cpu),
                  JitExecResult::Continue);
        m_cpu->materialize_lazy_flags();
        EXPECT_EQ(m_cpu->gpr.rax, 42u);
        EXPECT_EQ(m_cpu->gpr.rdx, 42u);
        EXPECT_EQ(m_cpu->gpr.rsp, 0x10000u);
        EXPECT_EQ(m_cpu->rip, 0x1008u);
        EXPECT_EQ(m_cpu->flags.rflags, 0x8D7u);
    };

    // Null call entry: retain the already-correct compiled_link_code path.
    void* const valid_call_entry = target->compiled_call_code;
    target->compiled_call_code = nullptr;
    run();

    // Exact-layout failure: transition through the established linked entry,
    // which flushes and reloads pins instead of entering the CALL landing pad.
    target->compiled_call_code = valid_call_entry;
    const u64 valid_layout = target->compiled_pin_layout;
    target->compiled_pin_layout ^= 1u;
    run();
    target->compiled_pin_layout = valid_layout;

    // L1 miss: return to the dispatcher with the committed guest CALL state.
    cache.clear();
    m_cpu->init_long_mode();
    m_cpu->gpr.rsp = 0x10000;
    m_cpu->rip = 0x1000;
    m_cpu->flags.rflags = 0x8D7u;
    EXPECT_EQ(test::execute_like_dispatcher(jit, *caller_compiled, *m_cpu), JitExecResult::Continue);
    m_cpu->materialize_lazy_flags();
    EXPECT_EQ(m_cpu->rip, 0x2000u);
    EXPECT_EQ(m_cpu->gpr.rsp, 0xFFF8u);
    EXPECT_EQ(m_memory->read_u64(0xFFF8).value_or(0), 0x1005u);
    EXPECT_EQ(m_cpu->flags.rflags, 0x8D7u);
}
