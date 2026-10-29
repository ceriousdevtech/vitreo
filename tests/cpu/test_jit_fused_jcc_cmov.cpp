// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT execution tests — flag producer fused with Jcc / CMOVcc

#include "jit_exec_test_fixture.hpp"

TEST_F(JitExecTest, InternalForwardFusedJccMatchesInterpreterForAllConditions) {
    const std::pair<u32, u32> pairs[] = {
        {5, 5}, {5, 3}, {3, 5}, {0x80000000u, 1},
        {0xFFFFFFFFu, 1}, {0, 0x80000000u},
        {0x7FFFFFFFu, 0xFFFFFFFFu},
    };

    for (u8 jcc = 0x70; jcc <= 0x7F; ++jcc) {
        for (auto [eax, ebx] : pairs) {
            const u8 code[] = {
                0x39, 0xD8,                         // cmp eax,ebx
                jcc, 0x05,                          // jcc target
                0xB9, 0x01, 0x00, 0x00, 0x00,     // mov ecx,1
                0xBA, 0x02, 0x00, 0x00, 0x00,     // target: mov edx,2
            };
            BasicBlock block = build_block(code, sizeof(code));
            ASSERT_EQ(block.instruction_count, 4u);

            // Seed the reference with the SAME sentinels and step it to the
            // end of the block: taken needs three instructions (cmp, jcc,
            // mov edx), fall-through four.
            CpuState reference;
            reference.init_protected_mode();
            reference.gpr.set32(ESP, 0x10000);
            reference.gpr.set32(EAX, eax);
            reference.gpr.set32(EBX, ebx);
            reference.gpr.set32(ECX, 0xCCCCCCCCu);
            reference.gpr.set32(EDX, 0xDDDDDDDDu);
            reference.rip = 0x1000;
            Interpreter interpreter(reference, *m_memory);
            (void)interpreter.run(4);
            ASSERT_EQ(reference.rip, 0x100Eu)
                << "both paths converge on the instruction after the block";

            CpuState actual;
            actual.init_protected_mode();
            actual.gpr.set32(ESP, 0x10000);
            actual.gpr.set32(EAX, eax);
            actual.gpr.set32(EBX, ebx);
            actual.gpr.set32(ECX, 0xCCCCCCCCu);
            actual.gpr.set32(EDX, 0xDDDDDDDDu);
            actual.rip = 0x1000;

            JitCompiler jit(*m_memory);
            CompiledBlock* compiled = jit.compile(block);
            ASSERT_NE(compiled, nullptr);
            EXPECT_EQ(test::execute_like_dispatcher(jit, *compiled, actual), JitExecResult::Continue);

            // How MUCH of the block one entry performs is the JIT's choice,
            // not a semantic property: a taken forward Jcc whose target is
            // in-block takes an internal B.cond and runs to the block's end
            // when the condition fuses (2026-09-06), and still leaves at the
            // target when it does not -- parity conditions never fuse, so both
            // shapes occur in this very loop. Pinning either one is what made
            // the earlier version of this test assert an implementation
            // detail. Drive any remainder through the interpreter and compare
            // the architectural state where the two must agree.
            if (actual.rip != reference.rip) {
                Interpreter finish(actual, *m_memory);
                (void)finish.run(4);
            }
            EXPECT_EQ(actual.rip, reference.rip);
            EXPECT_EQ(actual.gpr.get32(ECX), reference.gpr.get32(ECX));
            EXPECT_EQ(actual.gpr.get32(EDX), reference.gpr.get32(EDX));
        }
    }
}

TEST_F(JitExecTest, AddThenFusedJccMatchesInterpreterForAllConditions) {
    // ADD arms NzcvState::Add, which survives a reg-reg MOV, so a following Jcc
    // fuses off the ADDS NZCV. For addition x86 CF == ARM64 C (unlike SUB where
    // they invert), so fused_arm_cond must map the unsigned conditions
    // (B/NB/BE/A) differently. This exercises every condition against the
    // interpreter, taken and fall-through, for operand pairs that vary CF/OF/
    // SF/ZF/PF.
    const std::pair<u32, u32> pairs[] = {
        {5, 5}, {5, 3}, {3, 5}, {0x80000000u, 1},
        {0xFFFFFFFFu, 1}, {0xFFFFFFFFu, 0xFFFFFFFFu},
        {0, 0x80000000u}, {0x7FFFFFFFu, 1}, {0x7FFFFFFFu, 0x7FFFFFFFu},
    };

    for (u8 jcc = 0x70; jcc <= 0x7F; ++jcc) {
        for (auto [eax, ebx] : pairs) {
            // Terminal Jcc so fusion fires (embedded Jcc uses the helper path).
            //   0x1000: 01 D8   add eax,ebx
            //   0x1002: 89 D7   mov edi,edx   (reg-reg, NZCV-neutral)
            //   0x1004: jcc 02  → taken target 0x1008, fallthrough 0x1006
            const u8 code[] = {
                0x01, 0xD8,
                0x89, 0xD7,
                jcc, 0x02,
            };
            BasicBlock block = build_block(code, sizeof(code));
            ASSERT_EQ(block.instruction_count, 3u);

            CpuState reference;
            reference.init_protected_mode();
            reference.gpr.set32(ESP, 0x10000);
            reference.gpr.set32(EAX, eax);
            reference.gpr.set32(EBX, ebx);
            reference.rip = 0x1000;
            Interpreter interpreter(reference, *m_memory);
            (void)interpreter.run(3);              // add, mov, jcc
            const bool taken = reference.rip == 0x1008u;

            CpuState actual;
            actual.init_protected_mode();
            actual.gpr.set32(ESP, 0x10000);
            actual.gpr.set32(EAX, eax);
            actual.gpr.set32(EBX, ebx);
            actual.rip = 0x1000;

            JitCompiler jit(*m_memory);
            CompiledBlock* compiled = jit.compile(block);
            ASSERT_NE(compiled, nullptr);
            EXPECT_EQ(test::execute_like_dispatcher(jit, *compiled, actual), JitExecResult::Continue);

            EXPECT_EQ(actual.rip, reference.rip)
                << "jcc=0x" << std::hex << static_cast<int>(jcc)
                << " eax=0x" << eax << " ebx=0x" << ebx
                << " interp_taken=" << std::dec << taken;
        }
    }
}

TEST_F(JitExecTest, AddMemThenFusedJccMatchesInterpreter) {
    // ADD [mem] used to drop NzcvState because the store's bounds SUBS
    // clobbered NZCV. The store now spills NZCV so JL fuses.
    //   0x1000: 01 1E        add [esi], ebx
    //   0x1002: 7C 02        jl +2
    const u8 code[] = {0x01, 0x1E, 0x7C, 0x02};
    BasicBlock block = build_block(code, sizeof(code));
    ASSERT_EQ(block.instruction_count, 2u);

    const std::pair<u32, u32> pairs[] = {
        {5, 5}, {5, 3}, {0x7FFFFFFFu, 1}, {0x80000000u, 1},
    };
    for (auto [mem, ebx] : pairs) {
        ASSERT_TRUE(m_memory->write_u32(0x2000, mem));

        CpuState reference;
        reference.init_protected_mode();
        reference.gpr.set32(ESP, 0x10000);
        reference.gpr.set32(ESI, 0x2000);
        reference.gpr.set32(EBX, ebx);
        reference.rip = 0x1000;
        Interpreter interpreter(reference, *m_memory);
        (void)interpreter.run(2);

        ASSERT_TRUE(m_memory->write_u32(0x2000, mem));
        CpuState actual;
        actual.init_protected_mode();
        actual.gpr.set32(ESP, 0x10000);
        actual.gpr.set32(ESI, 0x2000);
        actual.gpr.set32(EBX, ebx);
        actual.rip = 0x1000;

        JitCompiler jit(*m_memory);
        CompiledBlock* compiled = jit.compile(block);
        ASSERT_NE(compiled, nullptr);
        EXPECT_EQ(test::execute_like_dispatcher(jit, *compiled, actual), JitExecResult::Continue);
        EXPECT_EQ(actual.rip, reference.rip)
            << "mem=0x" << std::hex << mem << " ebx=0x" << ebx;
        auto got = m_memory->read_u32(0x2000);
        ASSERT_TRUE(got);
        EXPECT_EQ(got.value(), mem + ebx);
    }
}

TEST_F(JitExecTest, CmpThenCmovzMatchesInterpreterTakenAndNotTaken) {
    // Intel SDM Vol. 2 CMOVcc: dest := src iff ZF=1; EFLAGS unchanged.
    // Catalog shape: cmp eax,eax; cmovz edx,ebx — fused CSEL from live NZCV.
    const u8 taken[] = {
        0x39, 0xC0,             // cmp eax, eax
        0x0F, 0x44, 0xD3,       // cmovz edx, ebx
    };
    const u8 not_taken[] = {
        0x39, 0xC8,             // cmp eax, ecx
        0x0F, 0x44, 0xD3,       // cmovz edx, ebx
    };
    const struct {
        const u8* code;
        usize len;
        u32 eax;
        u32 ecx;
        u32 edx;
        u32 ebx;
    } cases[] = {
        {taken, sizeof(taken), 7, 0, 0x11111111u, 0x22222222u},
        {not_taken, sizeof(not_taken), 5, 3, 0x11111111u, 0x22222222u},
        {not_taken, sizeof(not_taken), 0, 1, 0xAAAAAAAAu, 0xBBBBBBBBu},
        {taken, sizeof(taken), 0, 9, 0, 0xFFFFFFFFu},
    };
    for (const auto& c : cases) {
        ASSERT_TRUE(m_memory->write_bytes(0x1000, c.code, c.len));
        BasicBlock block = build_block(c.code, c.len);
        ASSERT_EQ(block.instruction_count, 2u);

        CpuState reference;
        reference.init_protected_mode();
        reference.gpr.set32(ESP, 0x10000);
        reference.gpr.set32(EAX, c.eax);
        reference.gpr.set32(ECX, c.ecx);
        reference.gpr.set32(EDX, c.edx);
        reference.gpr.set32(EBX, c.ebx);
        reference.rip = 0x1000;
        Interpreter interpreter(reference, *m_memory);
        (void)interpreter.run(2);
        reference.materialize_lazy_flags();

        CpuState actual;
        actual.init_protected_mode();
        actual.gpr.set32(ESP, 0x10000);
        actual.gpr.set32(EAX, c.eax);
        actual.gpr.set32(ECX, c.ecx);
        actual.gpr.set32(EDX, c.edx);
        actual.gpr.set32(EBX, c.ebx);
        actual.rip = 0x1000;

        JitCompiler jit(*m_memory);
        CompiledBlock* compiled = jit.compile(block);
        ASSERT_NE(compiled, nullptr);
        EXPECT_EQ(test::execute_like_dispatcher(jit, *compiled, actual), JitExecResult::Continue);
        actual.materialize_lazy_flags();

        EXPECT_EQ(actual.gpr.get32(EDX), reference.gpr.get32(EDX))
            << "eax=0x" << std::hex << c.eax << " ecx=0x" << c.ecx;
        EXPECT_EQ(actual.gpr.get32(EBX), reference.gpr.get32(EBX));
        EXPECT_EQ(actual.gpr.get32(EAX), reference.gpr.get32(EAX));
        EXPECT_EQ(actual.flags.zf(), reference.flags.zf());
        EXPECT_EQ(actual.flags.cf(), reference.flags.cf());
        EXPECT_EQ(actual.flags.sf(), reference.flags.sf());
        EXPECT_EQ(actual.flags.of(), reference.flags.of());
        EXPECT_EQ(actual.rip, reference.rip);
    }
}

TEST_F(JitExecTest, CmpThenCmovzR16MatchesInterpreterTakenAndNotTaken) {
    // Intel SDM Vol. 2 CMOVcc r16: move only low 16 bits; EFLAGS unchanged.
    const u8 taken[] = {
        0x66, 0x39, 0xC0,       // cmp ax, ax
        0x66, 0x0F, 0x44, 0xD3, // cmovz dx, bx
    };
    const u8 not_taken[] = {
        0x66, 0x39, 0xC8,       // cmp ax, cx
        0x66, 0x0F, 0x44, 0xD3, // cmovz dx, bx
    };
    const struct {
        const u8* code;
        usize len;
        u32 eax;
        u32 ecx;
        u32 edx;
        u32 ebx;
    } cases[] = {
        {taken, sizeof(taken), 0xAAAA1111u, 0, 0xCCCC2222u, 0xBBBB3333u},
        {not_taken, sizeof(not_taken), 0xAAAA1111u, 0x00002222u,
         0xCCCC2222u, 0xBBBB3333u},
    };
    for (const auto& c : cases) {
        ASSERT_TRUE(m_memory->write_bytes(0x1000, c.code, c.len));
        BasicBlock block = build_block(c.code, c.len);
        ASSERT_EQ(block.instruction_count, 2u);

        CpuState reference;
        reference.init_protected_mode();
        reference.gpr.set32(ESP, 0x10000);
        reference.gpr.set32(EAX, c.eax);
        reference.gpr.set32(ECX, c.ecx);
        reference.gpr.set32(EDX, c.edx);
        reference.gpr.set32(EBX, c.ebx);
        reference.rip = 0x1000;
        Interpreter interpreter(reference, *m_memory);
        (void)interpreter.run(2);
        reference.materialize_lazy_flags();

        CpuState actual;
        actual.init_protected_mode();
        actual.gpr.set32(ESP, 0x10000);
        actual.gpr.set32(EAX, c.eax);
        actual.gpr.set32(ECX, c.ecx);
        actual.gpr.set32(EDX, c.edx);
        actual.gpr.set32(EBX, c.ebx);
        actual.rip = 0x1000;

        JitCompiler jit(*m_memory);
        CompiledBlock* compiled = jit.compile(block);
        ASSERT_NE(compiled, nullptr);
        EXPECT_EQ(test::execute_like_dispatcher(jit, *compiled, actual), JitExecResult::Continue);
        actual.materialize_lazy_flags();

        EXPECT_EQ(actual.gpr.get32(EDX), reference.gpr.get32(EDX));
        EXPECT_EQ(actual.flags.zf(), reference.flags.zf());
        EXPECT_EQ(actual.rip, reference.rip);
    }
}

TEST_F(JitExecTest, CmpLeaMovThenFusedJccMatchesInterpreter) {
    // CMP; LEA; MOV r,r; JNZ — LEA used to drop m_flags.nzcv so JNZ hit the helper.
    //   0x1000: 39 D8        cmp eax, ebx
    //   0x1002: 8D 16        lea edx, [esi]
    //   0x1004: 8B C8        mov ecx, eax
    //   0x1006: 75 02        jnz +2
    const u8 code[] = {0x39, 0xD8, 0x8D, 0x16, 0x8B, 0xC8, 0x75, 0x02};
    BasicBlock block = build_block(code, sizeof(code));
    ASSERT_EQ(block.instruction_count, 4u);

    const std::pair<u32, u32> pairs[] = {
        {5, 5}, {3, 5}, {0, 0}, {1, 0},
    };
    for (auto [eax, ebx] : pairs) {
        CpuState reference;
        reference.init_protected_mode();
        reference.gpr.set32(ESP, 0x10000);
        reference.gpr.set32(EAX, eax);
        reference.gpr.set32(EBX, ebx);
        reference.gpr.set32(ESI, 0x2000);
        reference.rip = 0x1000;
        Interpreter interpreter(reference, *m_memory);
        (void)interpreter.run(4);

        CpuState actual;
        actual.init_protected_mode();
        actual.gpr.set32(ESP, 0x10000);
        actual.gpr.set32(EAX, eax);
        actual.gpr.set32(EBX, ebx);
        actual.gpr.set32(ESI, 0x2000);
        actual.rip = 0x1000;

        JitCompiler jit(*m_memory);
        CompiledBlock* compiled = jit.compile(block);
        ASSERT_NE(compiled, nullptr);
        EXPECT_EQ(test::execute_like_dispatcher(jit, *compiled, actual), JitExecResult::Continue);
        EXPECT_EQ(actual.rip, reference.rip)
            << "eax=0x" << std::hex << eax << " ebx=0x" << ebx;
        EXPECT_EQ(actual.gpr.get32(EDX), reference.gpr.get32(EDX));
        EXPECT_EQ(actual.gpr.get32(ECX), reference.gpr.get32(ECX));
    }
}


TEST_F(JitExecTest, CmpMovMemThenFusedJccMatchesInterpreter) {
    // CMP; MOV [mem], eax; JL — store used to kill CMP NZCV.
    //   0x1000: 39 D8        cmp eax, ebx
    //   0x1002: 89 06        mov [esi], eax
    //   0x1004: 7C 02        jl +2
    const u8 code[] = {0x39, 0xD8, 0x89, 0x06, 0x7C, 0x02};
    BasicBlock block = build_block(code, sizeof(code));
    ASSERT_EQ(block.instruction_count, 3u);

    const std::pair<u32, u32> pairs[] = {
        {5, 5}, {3, 5}, {0x80000000u, 1}, {1, 0x80000000u},
    };
    for (auto [eax, ebx] : pairs) {
        CpuState reference;
        reference.init_protected_mode();
        reference.gpr.set32(ESP, 0x10000);
        reference.gpr.set32(EAX, eax);
        reference.gpr.set32(EBX, ebx);
        reference.gpr.set32(ESI, 0x2000);
        reference.rip = 0x1000;
        Interpreter interpreter(reference, *m_memory);
        (void)interpreter.run(3);

        CpuState actual;
        actual.init_protected_mode();
        actual.gpr.set32(ESP, 0x10000);
        actual.gpr.set32(EAX, eax);
        actual.gpr.set32(EBX, ebx);
        actual.gpr.set32(ESI, 0x2000);
        actual.rip = 0x1000;

        JitCompiler jit(*m_memory);
        CompiledBlock* compiled = jit.compile(block);
        ASSERT_NE(compiled, nullptr);
        EXPECT_EQ(test::execute_like_dispatcher(jit, *compiled, actual), JitExecResult::Continue);
        EXPECT_EQ(actual.rip, reference.rip)
            << "eax=0x" << std::hex << eax << " ebx=0x" << ebx;
    }
}

TEST_F(JitExecTest, CmpMemThenCmovlClampsAboveLimit) {
    // Intel SDM Vol. 2 CMOVL: dest := src iff SF != OF. qMin of 0x1000 and
    // 0x800 must yield 0x800; an always-taken CMOVL yields 0x1000.
    ASSERT_TRUE(m_memory->write_u32(0x3000, 0x1000u).is_ok());
    ASSERT_TRUE(m_memory->write_u32(0x3004, 0x800u).is_ok());
    const u8 code[] = {
        0xBA, 0x00, 0x30, 0x00, 0x00, // mov edx, 0x3000
        0xB8, 0x04, 0x30, 0x00, 0x00, // mov eax, 0x3004
        0x8B, 0x0A,                   // mov ecx, [edx]
        0x3B, 0x08,                   // cmp ecx, [eax]
        0x0F, 0x4C, 0xC2,             // cmovl eax, edx
        0x8B, 0x00,                   // mov eax, [eax]
    };
    ASSERT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0x800u);
    EXPECT_EQ(m_cpu->gpr.get32(ECX), 0x1000u);

    ASSERT_TRUE(m_memory->write_u32(0x3000, 0x100u).is_ok());
    m_cpu->gpr.set32(EAX, 0);
    m_cpu->gpr.set32(ECX, 0);
    m_cpu->gpr.set32(EDX, 0);
    m_cpu->rip = 0x1000;
    ASSERT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0x100u);
    EXPECT_EQ(m_cpu->gpr.get32(ECX), 0x100u);
}
