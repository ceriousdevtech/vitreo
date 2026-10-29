// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests — 64-bit long mode (CPU-048): guarded-UD2
// traces and forward-join state.

#include "jit_parity_64_fixture.hpp"

TEST_F(JitParity64Test, GuardedUd2TraceTakenEdgeStaysInBlock) {
    ScopedEnvironmentVariable guarded_traces(
        "VITREO_JIT_GUARDED_TERMINAL_TRACES", "1");
    ScopedEnvironmentVariable native_ud2("VITREO_JIT_NATIVE_UD2", "1");
    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    m_jit = std::make_unique<JitCompiler>(*m_memory);
    // cmp eax,0 ; je target ; ud2 ; mov ebx,0x12345678 ; jmp next
    static const u8 code[] = {
        0x83, 0xF8, 0x00, 0x74, 0x02, 0x0F, 0x0B,
        0xBB, 0x78, 0x56, 0x34, 0x12, 0xEB, 0x00,
    };
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    BasicBlock block = vitreo::cli::decode_basic_block(*m_memory, 0x1000, true);
    ASSERT_EQ(block.instruction_count, 5u);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    EXPECT_EQ(compiled->native_insns, block.instruction_count);
    EXPECT_EQ(compiled->delegate_insns, nullptr);
    EXPECT_EQ(compiled->decoded_owner, nullptr);

    CpuState cpu;
    init_cpu(cpu);
    cpu.gpr.set32(RAX, 0);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    EXPECT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.gpr.get32(RBX), 0x12345678u);
    EXPECT_EQ(cpu.rip, 0x1000u + sizeof(code));
}

TEST_F(JitParity64Test, GuardedUd2TraceUntakenEdgeRaisesAtTrap) {
    ScopedEnvironmentVariable guarded_traces(
        "VITREO_JIT_GUARDED_TERMINAL_TRACES", "1");
    ScopedEnvironmentVariable native_ud2("VITREO_JIT_NATIVE_UD2", "1");
    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    m_jit = std::make_unique<JitCompiler>(*m_memory);
    static const u8 code[] = {
        0x83, 0xF8, 0x00, 0x74, 0x02, 0x0F, 0x0B,
        0xBB, 0x78, 0x56, 0x34, 0x12, 0xEB, 0x00,
    };
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    BasicBlock block = vitreo::cli::decode_basic_block(*m_memory, 0x1000, true);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    CpuState cpu;
    init_cpu(cpu);
    cpu.gpr.set32(RAX, 3);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    ASSERT_TRUE(cpu.pending_raise.active);
    EXPECT_TRUE(cpu.pending_raise.from_helper);
    EXPECT_EQ(cpu.pending_raise.code, 0xC000001Du);
    EXPECT_EQ(cpu.pending_raise.flags, 0u);
    EXPECT_EQ(cpu.pending_raise.num_args, 0u);
    EXPECT_EQ(cpu.rip, 0x1005u);
    EXPECT_EQ(cpu.gpr.get32(RBX), 0u);
    EXPECT_EQ(cpu.flag_op, CpuState::FlagOp::None);
    // CMP 3,0: PF is set while CF/AF/ZF/SF/OF are clear.  This non-default
    // result proves the cold stub made the lazy record canonical before x64
    // exception context construction.
    constexpr u64 arithmetic_flags = (1u << 0) | (1u << 2) | (1u << 4) |
                                     (1u << 6) | (1u << 7) | (1u << 11);
    EXPECT_EQ(cpu.flags.rflags & arithmetic_flags, 1u << 2);
}

TEST_F(JitParity64Test, GuardedUd2ForwardJoinFeedsDecCarryAndJnzNzcv) {
    ScopedEnvironmentVariable guarded_traces(
        "VITREO_JIT_GUARDED_TERMINAL_TRACES", "1");
    ScopedEnvironmentVariable native_ud2("VITREO_JIT_NATIVE_UD2", "1");
    ScopedEnvironmentVariable forward_join(
        "VITREO_JIT_FORWARD_JOIN_STATE", "1");
    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    m_jit = std::make_unique<JitCompiler>(*m_memory);

    // cmp eax,1 ; jne target ; ud2 ; target: dec rbx ; jnz next
    // EAX=0 takes the guarded edge with CF=1. DEC must preserve that CF while
    // publishing its zero result to the terminal JNZ.
    static const u8 code[] = {
        0x83, 0xF8, 0x01, 0x75, 0x02, 0x0F, 0x0B,
        0x48, 0xFF, 0xCB, 0x75, 0x00,
    };
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    BasicBlock block = vitreo::cli::decode_basic_block(*m_memory, 0x1000, true);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    EXPECT_EQ(compiled->native_insns, block.instruction_count);
    EXPECT_EQ(compiled->delegate_insns, nullptr);

    CpuState cpu;
    init_cpu(cpu);
    cpu.gpr.rax = 0;
    cpu.gpr.rbx = 1;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    EXPECT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.gpr.rbx, 0u);
    EXPECT_EQ(cpu.rip, 0x1000u + sizeof(code));
    cpu.materialize_lazy_flags();
    EXPECT_TRUE(cpu.flags.cf());
    EXPECT_TRUE(cpu.flags.zf());

    // CMP 2,1 takes the same guarded edge with CF=0.  Reusing the compiled
    // trace proves the join does not accidentally manufacture carry when DEC
    // replaces the other arithmetic flags.
    CpuState clear_carry_cpu;
    init_cpu(clear_carry_cpu);
    clear_carry_cpu.gpr.rax = 2;
    clear_carry_cpu.gpr.rbx = 1;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, clear_carry_cpu),
              JitExecResult::Continue);
    EXPECT_FALSE(clear_carry_cpu.pending_raise.active);
    EXPECT_EQ(clear_carry_cpu.gpr.rbx, 0u);
    EXPECT_EQ(clear_carry_cpu.rip, 0x1000u + sizeof(code));
    clear_carry_cpu.materialize_lazy_flags();
    EXPECT_FALSE(clear_carry_cpu.flags.cf());
    EXPECT_TRUE(clear_carry_cpu.flags.zf());
}

TEST_F(JitParity64Test, GuardedUd2ForwardJoinFeedsIncCarryAndJnz) {
    ScopedEnvironmentVariable guarded_traces(
        "VITREO_JIT_GUARDED_TERMINAL_TRACES", "1");
    ScopedEnvironmentVariable native_ud2("VITREO_JIT_NATIVE_UD2", "1");
    ScopedEnvironmentVariable forward_join(
        "VITREO_JIT_FORWARD_JOIN_STATE", "1");
    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    m_jit = std::make_unique<JitCompiler>(*m_memory);

    // cmp eax,1 ; jne target ; ud2 ; target: inc rbx ; jnz next
    // INC replaces the result flags consumed by JNZ but must preserve the CF
    // that arrived through the guarded forward join.
    static const u8 code[] = {
        0x83, 0xF8, 0x01, 0x75, 0x02, 0x0F, 0x0B,
        0x48, 0xFF, 0xC3, 0x75, 0x00,
    };
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    BasicBlock block = vitreo::cli::decode_basic_block(*m_memory, 0x1000, true);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    EXPECT_EQ(compiled->native_insns, block.instruction_count);
    EXPECT_EQ(compiled->delegate_insns, nullptr);

    struct Case {
        u64 rax;
        u64 rbx;
        u64 expected_rbx;
        u64 expected_rip;
        bool expected_cf;
        bool expected_zf;
    };
    static constexpr Case cases[] = {
        {0, ~u64{0}, 0, 0x1000u + sizeof(code), true, true},
        {2, 0,       1, 0x1000u + sizeof(code), false, false},
    };
    for (const auto& test : cases) {
        SCOPED_TRACE(::testing::Message()
                     << "RAX=" << test.rax << " RBX=" << test.rbx);
        CpuState cpu;
        init_cpu(cpu);
        cpu.gpr.rax = test.rax;
        cpu.gpr.rbx = test.rbx;
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
        EXPECT_FALSE(cpu.pending_raise.active);
        EXPECT_EQ(cpu.gpr.rbx, test.expected_rbx);
        EXPECT_EQ(cpu.rip, test.expected_rip);
        cpu.materialize_lazy_flags();
        EXPECT_EQ(cpu.flags.cf(), test.expected_cf);
        EXPECT_EQ(cpu.flags.zf(), test.expected_zf);
    }
}

TEST_F(JitParity64Test, ForwardJoinStateDecFeedsJccConditionMatrix) {
    ScopedEnvironmentVariable forward_join(
        "VITREO_JIT_FORWARD_JOIN_STATE", "1");
    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    m_jit = std::make_unique<JitCompiler>(*m_memory);

    // Intel SDM Vol. 1, Appendix B and Vol. 2 DEC/Jcc: DEC preserves CF and
    // defines OF/SF/ZF/AF/PF from its result. Exercise both outcomes for every
    // flag-based Jcc condition so forward-join publication cannot substitute
    // ARM carry, omit parity, or lose the signed overflow relationship.
    struct Case {
        const char* name;
        u8 condition;
        u64 initial_rbx;
        bool initial_cf;
        bool branch_taken;
    };
    static constexpr Case cases[] = {
        {"JO taken",     0x70, 0x8000000000000000ull, false, true},
        {"JO untaken",   0x70, 2, false, false},
        {"JNO taken",    0x71, 2, false, true},
        {"JNO untaken",  0x71, 0x8000000000000000ull, false, false},
        {"JB taken",     0x72, 2, true, true},
        {"JB untaken",   0x72, 2, false, false},
        {"JNB taken",    0x73, 2, false, true},
        {"JNB untaken",  0x73, 2, true, false},
        {"JZ taken",     0x74, 1, false, true},
        {"JZ untaken",   0x74, 2, false, false},
        {"JNZ taken",    0x75, 2, false, true},
        {"JNZ untaken",  0x75, 1, false, false},
        {"JBE CF taken", 0x76, 2, true, true},
        {"JBE ZF taken", 0x76, 1, false, true},
        {"JBE untaken",  0x76, 2, false, false},
        {"JA taken",     0x77, 2, false, true},
        {"JA CF untaken",0x77, 2, true, false},
        {"JA ZF untaken",0x77, 1, false, false},
        {"JS taken",     0x78, 0, false, true},
        {"JS untaken",   0x78, 1, false, false},
        {"JNS taken",    0x79, 1, false, true},
        {"JNS untaken",  0x79, 0, false, false},
        {"JP taken",     0x7A, 1, false, true},
        {"JP untaken",   0x7A, 2, false, false},
        {"JNP taken",    0x7B, 2, false, true},
        {"JNP untaken",  0x7B, 1, false, false},
        {"JL taken",     0x7C, 0, false, true},
        {"JL untaken",   0x7C, 2, false, false},
        {"JGE taken",    0x7D, 2, false, true},
        {"JGE untaken",  0x7D, 0, false, false},
        {"JLE ZF taken", 0x7E, 1, false, true},
        {"JLE SF taken", 0x7E, 0, false, true},
        {"JLE untaken",  0x7E, 2, false, false},
        {"JG taken",     0x7F, 2, false, true},
        {"JG ZF untaken",0x7F, 1, false, false},
        {"JG SF untaken",0x7F, 0, false, false},
    };
    for (const auto& test : cases) {
        SCOPED_TRACE(test.name);
        // Every row changes the bytes at 0x1000. Use an independent compiler
        // so the test's condition matrix does not trip the deliberate
        // per-address self-modifying-code churn limit after eight encodings.
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        const u8 code[] = {
            0x48, 0xFF, 0xCB,       // dec rbx
            test.condition, 0x02,    // Jcc +2
        };
        check_parity(code, sizeof(code),
            [&](CpuState& cpu) {
                cpu.gpr.rbx = test.initial_rbx;
                cpu.flags.set_cf(test.initial_cf);
            },
            /*check_flags=*/true, /*check_xmm=*/false,
            /*require_jit=*/true);
        ASSERT_TRUE(m_last_parity_ok);
        EXPECT_EQ(m_last_jit_cpu.rip,
                  0x1000u + sizeof(code) + (test.branch_taken ? 2u : 0u));
        EXPECT_EQ(m_last_jit_cpu.flags.cf(), test.initial_cf);
    }
}

TEST_F(JitParity64Test, GuardedUd2ForwardJoinKeepsLoopHeaderConservative) {
    ScopedEnvironmentVariable guarded_traces(
        "VITREO_JIT_GUARDED_TERMINAL_TRACES", "1");
    ScopedEnvironmentVariable native_ud2("VITREO_JIT_NATIVE_UD2", "1");
    ScopedEnvironmentVariable forward_join(
        "VITREO_JIT_FORWARD_JOIN_STATE", "1");
    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    m_jit = std::make_unique<JitCompiler>(*m_memory);

    // cmp rax,1 ; jne loop ; ud2 ; loop: dec rbx ; jnz loop
    // The loop header has both the guarded CMP edge and the DEC backedge.
    // Its flag provenance is therefore heterogeneous even though the forward
    // edge is the only path around UD2.
    static const u8 code[] = {
        0x48, 0x83, 0xF8, 0x01, 0x75, 0x02, 0x0F, 0x0B,
        0x48, 0xFF, 0xCB, 0x0F, 0x85, 0xF7, 0xFF, 0xFF, 0xFF,
    };
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    BasicBlock block = vitreo::cli::decode_basic_block(*m_memory, 0x1000, true);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    EXPECT_EQ(compiled->native_insns, block.instruction_count);
    EXPECT_EQ(compiled->delegate_insns, nullptr);

    CpuState cpu;
    init_cpu(cpu);
    cpu.gpr.rax = 0;
    cpu.gpr.rbx = 3;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    EXPECT_FALSE(cpu.pending_raise.active);
    // Arbitrary x64 mid-block backedges deliberately return through the
    // dispatcher.  Resume at the loop header; that block can then use its
    // block-start backedge without inheriting the guarded CMP provenance.
    EXPECT_EQ(cpu.gpr.rbx, 2u);
    EXPECT_EQ(cpu.rip, 0x1008u);
    BasicBlock loop = vitreo::cli::decode_basic_block(*m_memory, cpu.rip, true);
    CompiledBlock* compiled_loop = m_jit->compile(loop);
    ASSERT_NE(compiled_loop, nullptr);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled_loop, cpu), JitExecResult::Continue);
    EXPECT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.gpr.rbx, 0u);
    EXPECT_EQ(cpu.rip, 0x1000u + sizeof(code));
    cpu.materialize_lazy_flags();
    EXPECT_TRUE(cpu.flags.cf());
    EXPECT_TRUE(cpu.flags.zf());
}

TEST_F(JitParity64Test, GuardedUd2ForwardJoinAcceptsHomogeneousSources) {
    ScopedEnvironmentVariable guarded_traces(
        "VITREO_JIT_GUARDED_TERMINAL_TRACES", "1");
    ScopedEnvironmentVariable native_ud2("VITREO_JIT_NATIVE_UD2", "1");
    ScopedEnvironmentVariable forward_join(
        "VITREO_JIT_FORWARD_JOIN_STATE", "1");
    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    m_jit = std::make_unique<JitCompiler>(*m_memory);

    // cmp eax,1 ; jne target ; cmp ebx,1 ; jne target ; ud2
    // target: adc rcx,0 ; jmp next
    // Both incoming edges carry the same Sub32/NZCV provenance, but their
    // runtime CF values differ.  The join may reuse the provenance class; it
    // must still consume the flags from the edge that was actually taken.
    static const u8 code[] = {
        0x83, 0xF8, 0x01, 0x75, 0x07,
        0x83, 0xFB, 0x01, 0x75, 0x02,
        0x0F, 0x0B,
        0x48, 0x83, 0xD1, 0x00, 0xEB, 0x00,
    };

    struct Case {
        const char* name;
        u32 eax;
        u32 ebx;
        u64 expected_rcx;
    };
    static constexpr Case cases[] = {
        {"first source, CF set", 0, 1, 11},
        {"second source, CF clear", 1, 2, 10},
    };
    for (const Case& test : cases) {
        SCOPED_TRACE(test.name);
        check_parity(code, sizeof(code), [&](CpuState& cpu) {
            cpu.gpr.set32(RAX, test.eax);
            cpu.gpr.set32(RBX, test.ebx);
            cpu.gpr.rcx = 10;
        }, /*check_flags=*/true, /*check_xmm=*/false,
           /*require_jit=*/true);
        ASSERT_TRUE(m_last_parity_ok);
        EXPECT_EQ(m_last_jit_cpu.gpr.rcx, test.expected_rcx);
        EXPECT_EQ(m_last_jit_cpu.rip, 0x1000u + sizeof(code));
    }
}

TEST_F(JitParity64Test, GuardedUd2ForwardJoinRejectsHeterogeneousSources) {
    ScopedEnvironmentVariable guarded_traces(
        "VITREO_JIT_GUARDED_TERMINAL_TRACES", "1");
    ScopedEnvironmentVariable native_ud2("VITREO_JIT_NATIVE_UD2", "1");
    ScopedEnvironmentVariable forward_join(
        "VITREO_JIT_FORWARD_JOIN_STATE", "1");
    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    m_jit = std::make_unique<JitCompiler>(*m_memory);

    // test eax,eax ; jz target ; cmp ebx,1 ; jne target ; ud2
    // target: adc rcx,0 ; jmp next
    // The first source carries Logic32/CF=0 while the second carries
    // Sub32/CF=1.  The target must reject a homogeneous host-state proof and
    // recover CF from the architectural lazy record for each incoming edge.
    static const u8 code[] = {
        0x85, 0xC0, 0x74, 0x07,
        0x83, 0xFB, 0x01, 0x75, 0x02,
        0x0F, 0x0B,
        0x48, 0x83, 0xD1, 0x00, 0xEB, 0x00,
    };

    struct Case {
        const char* name;
        u32 eax;
        u32 ebx;
        u64 expected_rcx;
    };
    static constexpr Case cases[] = {
        {"Logic32 source, CF clear", 0, 1, 10},
        {"Sub32 source, CF set", 1, 0, 11},
    };
    for (const Case& test : cases) {
        SCOPED_TRACE(test.name);
        check_parity(code, sizeof(code), [&](CpuState& cpu) {
            cpu.gpr.set32(RAX, test.eax);
            cpu.gpr.set32(RBX, test.ebx);
            cpu.gpr.rcx = 10;
        }, /*check_flags=*/true, /*check_xmm=*/false,
           /*require_jit=*/true);
        ASSERT_TRUE(m_last_parity_ok);
        EXPECT_EQ(m_last_jit_cpu.gpr.rcx, test.expected_rcx);
        EXPECT_EQ(m_last_jit_cpu.rip, 0x1000u + sizeof(code));
    }
}

TEST_F(JitParity64Test, GuardedUd2ForwardJoinFeedsEveryJccCondition) {
    ScopedEnvironmentVariable guarded_traces(
        "VITREO_JIT_GUARDED_TERMINAL_TRACES", "1");
    ScopedEnvironmentVariable native_ud2("VITREO_JIT_NATIVE_UD2", "1");
    ScopedEnvironmentVariable forward_join(
        "VITREO_JIT_FORWARD_JOIN_STATE", "1");
    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");

    // cmp eax,1 ; JZ/JNZ target ; ud2 ; target: Jcc +2
    // The guarding branch is chosen only to ensure the target is reached;
    // the second branch covers every Intel condition against the CMP flags
    // carried through the actual post-UD2 join.  Parity conditions exercise
    // the architectural lazy fallback because AArch64 has no PSTATE parity.
    struct Case {
        const char* name;
        u8 condition;
        u32 eax;
        bool branch_taken;
    };
    static constexpr Case cases[] = {
        {"JO",  0x70, 2, false}, {"JNO", 0x71, 2, true},
        {"JB",  0x72, 0, true},  {"JNB", 0x73, 2, true},
        {"JZ",  0x74, 1, true},  {"JNZ", 0x75, 2, true},
        {"JBE", 0x76, 0, true},  {"JA",  0x77, 2, true},
        {"JS",  0x78, 0, true},  {"JNS", 0x79, 2, true},
        {"JP",  0x7A, 0, true},  {"JNP", 0x7B, 2, true},
        {"JL",  0x7C, 0, true},  {"JGE", 0x7D, 2, true},
        {"JLE", 0x7E, 1, true},  {"JG",  0x7F, 2, true},
    };
    for (const Case& test : cases) {
        SCOPED_TRACE(test.name);
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        const u8 guard = test.eax == 1 ? 0x74 : 0x75;
        const u8 code[] = {
            0x83, 0xF8, 0x01, guard, 0x02, 0x0F, 0x0B,
            test.condition, 0x02,
        };
        check_parity(code, sizeof(code), [&](CpuState& cpu) {
            cpu.gpr.set32(RAX, test.eax);
        }, /*check_flags=*/true, /*check_xmm=*/false,
           /*require_jit=*/true);
        ASSERT_TRUE(m_last_parity_ok);
        EXPECT_EQ(m_last_jit_cpu.rip,
                  0x1000u + sizeof(code) + (test.branch_taken ? 2u : 0u));
    }
}

TEST_F(JitParity64Test, ForwardJoinDefaultAndExplicitOptOutHaveCorrectMemoIdentity) {
    struct MemoGuard {
        ~MemoGuard() {
            JitCompileMemo::disable_compile_memo();
            JitCompileMemo::clear_compile_memo();
        }
    } memo_guard;
    ScopedEnvironmentVariable guarded_traces(
        "VITREO_JIT_GUARDED_TERMINAL_TRACES", "1");
    ScopedEnvironmentVariable native_ud2("VITREO_JIT_NATIVE_UD2", "1");
    ScopedEnvironmentVariable forward_join_default(
        "VITREO_JIT_FORWARD_JOIN_STATE", nullptr);
    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    JitCompileMemo::clear_compile_memo();
    JitCompileMemo::enable_compile_memo();

    // Keep this declaration adjacent to the rollout assertion.  Change it to
    // true in the same patch that changes the constructor default; the memo
    // expectation then inverts and proves explicit =0 cannot reuse default-on
    // generated code.
    static constexpr bool kForwardJoinDefaultEnabled = true;
    static const u8 code[] = {
        0x83, 0xF8, 0x01, 0x75, 0x02, 0x0F, 0x0B,
        0x48, 0xFF, 0xCB, 0x75, 0x00,
    };
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    BasicBlock block = vitreo::cli::decode_basic_block(*m_memory, 0x1000, true);

    JitCompiler default_jit(*m_memory);
    CompiledBlock* default_block = default_jit.compile(block);
    ASSERT_NE(default_block, nullptr);
    void* default_code = default_block->code;

    CpuState default_cpu;
    init_cpu(default_cpu);
    default_cpu.gpr.rax = 0;
    default_cpu.gpr.rbx = 1;
    EXPECT_EQ(test::execute_like_dispatcher(default_jit, *default_block, default_cpu),
              JitExecResult::Continue);

    ScopedEnvironmentVariable forward_join_off(
        "VITREO_JIT_FORWARD_JOIN_STATE", "0");
    JitCompiler opted_out_jit(*m_memory);
    CompiledBlock* opted_out = opted_out_jit.compile(block);
    ASSERT_NE(opted_out, nullptr);
    EXPECT_EQ(default_code != opted_out->code, kForwardJoinDefaultEnabled)
        << "compile memo identity does not match the documented default";

    CpuState opted_out_cpu;
    init_cpu(opted_out_cpu);
    opted_out_cpu.gpr.rax = 0;
    opted_out_cpu.gpr.rbx = 1;
    EXPECT_EQ(test::execute_like_dispatcher(opted_out_jit, *opted_out, opted_out_cpu),
              JitExecResult::Continue);
    default_cpu.materialize_lazy_flags();
    opted_out_cpu.materialize_lazy_flags();
    EXPECT_EQ(default_cpu.gpr.rbx, opted_out_cpu.gpr.rbx);
    EXPECT_EQ(default_cpu.rip, opted_out_cpu.rip);
    EXPECT_EQ(default_cpu.flags.rflags, opted_out_cpu.flags.rflags);
    EXPECT_EQ(default_cpu.pending_raise.active,
              opted_out_cpu.pending_raise.active);
}

TEST_F(JitParity64Test, CompileMemoSeparatesNativeUd2Encoding) {
    struct MemoGuard {
        ~MemoGuard() {
            JitCompileMemo::disable_compile_memo();
            JitCompileMemo::clear_compile_memo();
        }
    } memo_guard;
    ScopedEnvironmentVariable guarded_traces(
        "VITREO_JIT_GUARDED_TERMINAL_TRACES", "1");
    ScopedEnvironmentVariable native_ud2_off("VITREO_JIT_NATIVE_UD2", "0");
    ScopedEnvironmentVariable forward_join_off(
        "VITREO_JIT_FORWARD_JOIN_STATE", "0");
    JitCompileMemo::clear_compile_memo();
    JitCompileMemo::enable_compile_memo();

    static const u8 code[] = {
        0x83, 0xF8, 0x00, 0x74, 0x02, 0x0F, 0x0B,
        0xBB, 0x78, 0x56, 0x34, 0x12, 0xEB, 0x00,
    };
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    BasicBlock block = vitreo::cli::decode_basic_block(*m_memory, 0x1000, true);
    ASSERT_EQ(block.instruction_count, 5u);

    JitCompiler delegated_jit(*m_memory);
    CompiledBlock* delegated = delegated_jit.compile(block);
    ASSERT_NE(delegated, nullptr);
    ASSERT_NE(delegated->delegate_insns, nullptr);
    void* delegated_code = delegated->code;

    {
        ScopedEnvironmentVariable native_ud2_on(
            "VITREO_JIT_NATIVE_UD2", "1");
        ScopedEnvironmentVariable no_delegate(
            "VITREO_JIT_NO_DELEGATE", "1");
        JitCompiler native_jit(*m_memory);
        CompiledBlock* native = native_jit.compile(block);
        ASSERT_NE(native, nullptr);
        EXPECT_NE(native->code, delegated_code)
            << "native and delegated UD2 translations shared one memo entry";
        EXPECT_EQ(native->native_insns, block.instruction_count);
        EXPECT_EQ(native->delegate_insns, nullptr);
        EXPECT_EQ(native->decoded_owner, nullptr);

        ScopedEnvironmentVariable forward_join_on(
            "VITREO_JIT_FORWARD_JOIN_STATE", "1");
        JitCompiler joined_jit(*m_memory);
        CompiledBlock* joined = joined_jit.compile(block);
        ASSERT_NE(joined, nullptr);
        EXPECT_NE(joined->code, native->code)
            << "forward-join state mode shared a memo entry with baseline";
        EXPECT_EQ(joined->delegate_insns, nullptr);
    }
}

TEST_F(JitParity64Test, NativeUd2DefaultsOnAndOptOutMatchesDelegate) {
    struct MemoGuard {
        ~MemoGuard() {
            JitCompileMemo::disable_compile_memo();
            JitCompileMemo::clear_compile_memo();
        }
    } memo_guard;
    ScopedEnvironmentVariable native_default("VITREO_JIT_NATIVE_UD2",
                                              nullptr);
    ScopedEnvironmentVariable forward_join_off(
        "VITREO_JIT_FORWARD_JOIN_STATE", "0");
    JitCompileMemo::clear_compile_memo();
    JitCompileMemo::enable_compile_memo();

    // sub rax,1 ; ud2. UD2 must preserve the SUB result and flags while
    // reporting the trap instruction's own RIP.
    static const u8 code[] = {
        0x48, 0x83, 0xE8, 0x01,
        0x0F, 0x0B,
    };
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    BasicBlock block = vitreo::cli::decode_basic_block(*m_memory, 0x1000, true);
    ASSERT_EQ(block.instruction_count, 2u);

    JitCompiler native_jit(*m_memory);
    CompiledBlock* native = native_jit.compile(block);
    ASSERT_NE(native, nullptr);
    EXPECT_EQ(native->native_insns, block.instruction_count);
    EXPECT_EQ(native->delegate_insns, nullptr);
    EXPECT_EQ(native->decoded_owner, nullptr);
    void* native_code = native->code;

    CpuState native_cpu;
    init_cpu(native_cpu);
    native_cpu.gpr.rax = 1;
    native_cpu.gpr.rbx = 0x1122334455667788ull;
    native_cpu.flags.rflags = 0x8D7u;
    EXPECT_EQ(test::execute_like_dispatcher(native_jit, *native, native_cpu), JitExecResult::Continue);

    ScopedEnvironmentVariable native_off("VITREO_JIT_NATIVE_UD2", "0");
    JitCompiler delegated_jit(*m_memory);
    CompiledBlock* delegated = delegated_jit.compile(block);
    ASSERT_NE(delegated, nullptr);
    EXPECT_NE(delegated->code, native_code)
        << "default-native and explicit delegate modes shared a memo entry";
    ASSERT_NE(delegated->delegate_insns, nullptr);
    ASSERT_NE(delegated->decoded_owner, nullptr);

    CpuState delegated_cpu;
    init_cpu(delegated_cpu);
    delegated_cpu.gpr.rax = 1;
    delegated_cpu.gpr.rbx = 0x1122334455667788ull;
    delegated_cpu.flags.rflags = 0x8D7u;
    EXPECT_EQ(test::execute_like_dispatcher(delegated_jit, *delegated, delegated_cpu),
              JitExecResult::Continue);

    ASSERT_TRUE(native_cpu.pending_raise.active);
    ASSERT_TRUE(delegated_cpu.pending_raise.active);
    EXPECT_EQ(native_cpu.pending_raise.code,
              delegated_cpu.pending_raise.code);
    EXPECT_EQ(native_cpu.pending_raise.flags,
              delegated_cpu.pending_raise.flags);
    EXPECT_EQ(native_cpu.pending_raise.num_args,
              delegated_cpu.pending_raise.num_args);
    EXPECT_EQ(native_cpu.pending_raise.from_helper,
              delegated_cpu.pending_raise.from_helper);
    EXPECT_EQ(native_cpu.rip, 0x1004u);
    EXPECT_EQ(native_cpu.rip, delegated_cpu.rip);
    EXPECT_EQ(native_cpu.gpr.rax, 0u);
    EXPECT_EQ(native_cpu.gpr.rax, delegated_cpu.gpr.rax);
    EXPECT_EQ(native_cpu.gpr.rbx, delegated_cpu.gpr.rbx);
    EXPECT_EQ(native_cpu.flag_op, CpuState::FlagOp::None);
    EXPECT_EQ(native_cpu.flag_op, delegated_cpu.flag_op);
    EXPECT_EQ(native_cpu.flags.rflags, delegated_cpu.flags.rflags);
}
