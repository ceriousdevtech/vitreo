// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT execution tests — Jcc-first blocks reading lazy flag sources

#include "jit_exec_test_fixture.hpp"

TEST_F(JitExecTest, JccFirstFromLazySub32MatchesInterpreter) {
    // Jcc-first, nzcv_kind cleared: reconstruct CMP from FlagOp::Sub32.
    const u8 jcc_code[] = {0x7D, 0x02};  // jge +2
    ASSERT_TRUE(m_memory->write_bytes(0x1002, jcc_code, sizeof(jcc_code)));

    BasicBlock jcc_block;
    jcc_block.start_addr = 0x1002;
    MemoryByteReader reader(jcc_code, sizeof(jcc_code));
    DecodedInstruction inst;
    ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode32, inst),
              DecodeError::Success);
    jcc_block.instructions.push_back(inst);
    jcc_block.byte_length = 2;
    jcc_block.instruction_count = 1;
    CompiledBlock* jcc_compiled = m_jit->compile(jcc_block);
    ASSERT_NE(jcc_compiled, nullptr);

    const std::pair<u32, u32> pairs[] = {
        {5, 3}, {3, 5}, {5, 5}, {0x80000000u, 1}, {1, 0x80000000u},
    };
    for (auto [eax, ebx] : pairs) {
        const u32 result = eax - ebx;
        CpuState reference;
        reference.init_protected_mode();
        reference.gpr.set32(ESP, 0x10000);
        reference.flag_op = CpuState::FlagOp::Sub32;
        reference.flag_src = ebx;
        reference.flag_dst = result;
        reference.cf_overlay = 0;
        reference.of_overlay = 0;
        reference.nzcv_kind = 0;
        reference.rip = 0x1002;
        const int take = reference.try_lazy_condition(0xD);
        ASSERT_GE(take, 0);
        const u64 want_rip = take ? 0x1006u : 0x1004u;

        m_cpu->init_protected_mode();
        m_cpu->gpr.set32(ESP, 0x10000);
        m_cpu->flag_op = CpuState::FlagOp::Sub32;
        m_cpu->flag_src = ebx;
        m_cpu->flag_dst = result;
        m_cpu->cf_overlay = 0;
        m_cpu->of_overlay = 0;
        m_cpu->nzcv_kind = 0;
        m_cpu->rip = 0x1002;
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *jcc_compiled, *m_cpu),
                  JitExecResult::Continue);
        EXPECT_EQ(m_cpu->rip, want_rip)
            << "eax=0x" << std::hex << eax << " ebx=0x" << ebx;
    }
}

TEST_F(JitExecTest, JccFirstFromLazyAnd32MatchesInterpreter) {
    // TEST EBP, EBP; JGE — the load-walk signed condition after And32.
    const u8 jcc_code[] = {0x7D, 0x02};  // jge +2
    ASSERT_TRUE(m_memory->write_bytes(0x1002, jcc_code, sizeof(jcc_code)));

    BasicBlock jcc_block;
    jcc_block.start_addr = 0x1002;
    MemoryByteReader reader(jcc_code, sizeof(jcc_code));
    DecodedInstruction inst;
    ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode32, inst),
              DecodeError::Success);
    jcc_block.instructions.push_back(inst);
    jcc_block.byte_length = 2;
    jcc_block.instruction_count = 1;
    CompiledBlock* jcc_compiled = m_jit->compile(jcc_block);
    ASSERT_NE(jcc_compiled, nullptr);

    const u32 ebps[] = {0u, 1u, 0x80000000u, 0xFFFFFFFFu};
    for (u32 ebp : ebps) {
        CpuState reference;
        reference.init_protected_mode();
        reference.gpr.set32(ESP, 0x10000);
        reference.flag_op = CpuState::FlagOp::And32;
        reference.flag_src = ebp;
        reference.flag_dst = ebp;
        reference.cf_overlay = 0;
        reference.of_overlay = 0;
        reference.nzcv_kind = 0;
        reference.rip = 0x1002;
        const int take = reference.try_lazy_condition(0xD);
        ASSERT_GE(take, 0);
        const u64 want_rip = take ? 0x1006u : 0x1004u;

        m_cpu->init_protected_mode();
        m_cpu->gpr.set32(ESP, 0x10000);
        m_cpu->flag_op = CpuState::FlagOp::And32;
        m_cpu->flag_src = ebp;
        m_cpu->flag_dst = ebp;
        m_cpu->cf_overlay = 0;
        m_cpu->of_overlay = 0;
        m_cpu->nzcv_kind = 0;
        m_cpu->rip = 0x1002;
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *jcc_compiled, *m_cpu),
                  JitExecResult::Continue);
        EXPECT_EQ(m_cpu->rip, want_rip) << "ebp=0x" << std::hex << ebp;
    }
}

TEST_F(JitExecTest, JccFirstFromLazySub32JbMatchesInterpreter) {
    const u8 jcc_code[] = {0x72, 0x02};  // jb +2
    ASSERT_TRUE(m_memory->write_bytes(0x1002, jcc_code, sizeof(jcc_code)));

    BasicBlock jcc_block;
    jcc_block.start_addr = 0x1002;
    MemoryByteReader reader(jcc_code, sizeof(jcc_code));
    DecodedInstruction inst;
    ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode32, inst),
              DecodeError::Success);
    jcc_block.instructions.push_back(inst);
    jcc_block.byte_length = 2;
    jcc_block.instruction_count = 1;
    CompiledBlock* jcc_compiled = m_jit->compile(jcc_block);
    ASSERT_NE(jcc_compiled, nullptr);

    const std::pair<u32, u32> pairs[] = {
        {5, 3}, {3, 5}, {0, 1}, {1, 0}, {0xFFFFFFFFu, 1},
    };
    for (auto [eax, ebx] : pairs) {
        const u32 result = eax - ebx;
        CpuState reference;
        reference.init_protected_mode();
        reference.flag_op = CpuState::FlagOp::Sub32;
        reference.flag_src = ebx;
        reference.flag_dst = result;
        reference.nzcv_kind = 0;
        const int take = reference.try_lazy_condition(0x2);
        ASSERT_GE(take, 0);
        const u64 want_rip = take ? 0x1006u : 0x1004u;

        m_cpu->init_protected_mode();
        m_cpu->flag_op = CpuState::FlagOp::Sub32;
        m_cpu->flag_src = ebx;
        m_cpu->flag_dst = result;
        m_cpu->nzcv_kind = 0;
        m_cpu->rip = 0x1002;
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *jcc_compiled, *m_cpu),
                  JitExecResult::Continue);
        EXPECT_EQ(m_cpu->rip, want_rip)
            << "eax=0x" << std::hex << eax << " ebx=0x" << ebx;
    }
}

TEST_F(JitExecTest, TestEdiJneCopyLoopStops) {
    // Literal-copy loop from the TS3 decompressor that GPs at 0x4e49a1
    // when JNE stays taken (ADD flags instead of TEST).
    const u8 loop[] = {
        0x8A, 0x19,        // mov bl, [ecx]
        0x83, 0xEF, 0x01,  // sub edi, 1
        0x88, 0x1E,        // mov [esi], bl
        0x83, 0xC6, 0x01,  // add esi, 1
        0x83, 0xC1, 0x01,  // add ecx, 1
        0x85, 0xFF,        // test edi, edi
        0x75, 0xEF,        // jne loop
    };
    ASSERT_EQ(sizeof(loop), 17u);

    const u8 src[] = {'A', 'B', 'C', 'D'};
    ASSERT_TRUE(m_memory->write_bytes(0x2000, src, sizeof(src)));
    const u8 z[8] = {};
    ASSERT_TRUE(m_memory->write_bytes(0x3000, z, sizeof(z)));

    m_cpu->gpr.set32(ECX, 0x2000);
    m_cpu->gpr.set32(ESI, 0x3000);
    m_cpu->gpr.set32(EDI, 3);
    m_cpu->gpr.set32(EBX, 0);
    m_cpu->rip = 0x1000;

    EXPECT_EQ(jit_run(loop, sizeof(loop)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->rip, 0x1000u + sizeof(loop));
    EXPECT_EQ(m_cpu->gpr.get32(EDI), 0u);
    EXPECT_EQ(m_cpu->gpr.get32(ESI), 0x3003u);
    EXPECT_EQ(m_cpu->gpr.get32(ECX), 0x2003u);

    u8 dest[4] = {};
    ASSERT_TRUE(m_memory->read_bytes(0x3000, dest, sizeof(dest)));
    EXPECT_EQ(dest[0], 'A');
    EXPECT_EQ(dest[1], 'B');
    EXPECT_EQ(dest[2], 'C');
    EXPECT_EQ(dest[3], 0);
}

TEST_F(JitExecTest, JccFirstFromLazyAnd8JsMatchesInterpreter) {
    // TEST r/m8 leaves FlagOp::And8. JS reads SF = bit 7, not ARM N after
    // a zero-extended ANDS (that path never took JS and overran a copy).
    const u8 jcc_code[] = {0x78, 0x02};  // js +2
    ASSERT_TRUE(m_memory->write_bytes(0x1002, jcc_code, sizeof(jcc_code)));

    BasicBlock jcc_block;
    jcc_block.start_addr = 0x1002;
    MemoryByteReader reader(jcc_code, sizeof(jcc_code));
    DecodedInstruction inst;
    ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode32, inst),
              DecodeError::Success);
    jcc_block.instructions.push_back(inst);
    jcc_block.byte_length = 2;
    jcc_block.instruction_count = 1;
    CompiledBlock* jcc_compiled = m_jit->compile(jcc_block);
    ASSERT_NE(jcc_compiled, nullptr);

    const u8 results[] = {0u, 1u, 0x7Fu, 0x80u, 0xFFu};
    for (u8 result : results) {
        CpuState reference;
        reference.init_protected_mode();
        reference.flag_op = CpuState::FlagOp::And8;
        reference.flag_src = 0xFF;
        reference.flag_dst = result;
        reference.nzcv_kind = 0;
        const int take = reference.try_lazy_condition(0x8);
        ASSERT_GE(take, 0);
        const u64 want_rip = take ? 0x1006u : 0x1004u;

        m_cpu->init_protected_mode();
        m_cpu->flag_op = CpuState::FlagOp::And8;
        m_cpu->flag_src = 0xFF;
        m_cpu->flag_dst = result;
        m_cpu->nzcv_kind = 0;
        m_cpu->rip = 0x1002;
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *jcc_compiled, *m_cpu),
                  JitExecResult::Continue);
        EXPECT_EQ(m_cpu->rip, want_rip)
            << "result=0x" << std::hex << static_cast<unsigned>(result);
    }
}

TEST_F(JitExecTest, TestDlJsSameBlockMatchesInterpreter) {
    const u8 code[] = {
        0x84, 0xD2,  // test dl, dl
        0x78, 0x01,  // js +1  (skip inc)
        0x40,        // inc eax
    };
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    const u8 dls[] = {0u, 1u, 0x7Fu, 0x80u, 0xFFu};
    for (u8 dl : dls) {
        m_cpu->init_protected_mode();
        m_cpu->gpr.set32(ESP, 0x10000);
        m_cpu->gpr.set32(EAX, 0);
        m_cpu->gpr.set32(EDX, dl);
        m_cpu->rip = 0x1000;
        CpuState reference = *m_cpu;
        Interpreter interp(reference, *m_memory);
        (void)interp.run(3);

        m_cpu->init_protected_mode();
        m_cpu->gpr.set32(ESP, 0x10000);
        m_cpu->gpr.set32(EAX, 0);
        m_cpu->gpr.set32(EDX, dl);
        m_cpu->rip = 0x1000;
        EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
        EXPECT_EQ(m_cpu->rip, reference.rip)
            << "dl=0x" << std::hex << static_cast<unsigned>(dl);
        EXPECT_EQ(m_cpu->gpr.get32(EAX), reference.gpr.get32(EAX))
            << "dl=0x" << std::hex << static_cast<unsigned>(dl);
    }
}

TEST_F(JitExecTest, TestCxJsSameBlockMatchesInterpreter) {
    // 16-bit TEST publishes NZCV via SXTH+ANDS so same-block JS fuses.
    // TEST CX (not AX): taken JS lands past INC; a following ADD [EAX],AL
    // must see EAX=0 so the interpreter comparison stays inside the snippet.
    const u8 code[] = {
        0x66, 0x85, 0xC9,  // test cx, cx
        0x78, 0x01,        // js +1  (skip inc)
        0x40,              // inc eax
    };
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    const u16 cxs[] = {0u, 1u, 0x7FFFu, 0x8000u, 0xFFFFu};
    for (u16 cx : cxs) {
        m_cpu->init_protected_mode();
        m_cpu->gpr.set32(ESP, 0x10000);
        m_cpu->gpr.set32(EAX, 0);
        m_cpu->gpr.set32(ECX, cx);
        m_cpu->rip = 0x1000;
        CpuState reference = *m_cpu;
        Interpreter interp(reference, *m_memory);
        (void)interp.run(3);

        m_cpu->init_protected_mode();
        m_cpu->gpr.set32(ESP, 0x10000);
        m_cpu->gpr.set32(EAX, 0);
        m_cpu->gpr.set32(ECX, cx);
        m_cpu->rip = 0x1000;
        EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
        EXPECT_EQ(m_cpu->rip, reference.rip)
            << "cx=0x" << std::hex << cx;
        EXPECT_EQ(m_cpu->gpr.get32(EAX), reference.gpr.get32(EAX))
            << "cx=0x" << std::hex << cx;
    }
}

TEST_F(JitExecTest, JccFirstFromLazyInc32MatchesInterpreter) {
    const u8 jcc_code[] = {0x75, 0x02};  // jnz +2
    ASSERT_TRUE(m_memory->write_bytes(0x1002, jcc_code, sizeof(jcc_code)));

    BasicBlock jcc_block;
    jcc_block.start_addr = 0x1002;
    MemoryByteReader reader(jcc_code, sizeof(jcc_code));
    DecodedInstruction inst;
    ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode32, inst),
              DecodeError::Success);
    jcc_block.instructions.push_back(inst);
    jcc_block.byte_length = 2;
    jcc_block.instruction_count = 1;
    CompiledBlock* jcc_compiled = m_jit->compile(jcc_block);
    ASSERT_NE(jcc_compiled, nullptr);

    const u32 results[] = {0u, 1u, 0xFFFFFFFFu};
    for (u32 result : results) {
        CpuState reference;
        reference.init_protected_mode();
        reference.flag_op = CpuState::FlagOp::Inc32;
        reference.flag_src = static_cast<u64>(result - 1u);
        reference.flag_dst = result;
        reference.nzcv_kind = 0;
        const int take = reference.try_lazy_condition(0x5);
        ASSERT_GE(take, 0);
        const u64 want_rip = take ? 0x1006u : 0x1004u;

        m_cpu->init_protected_mode();
        m_cpu->flag_op = CpuState::FlagOp::Inc32;
        m_cpu->flag_src = static_cast<u64>(result - 1u);
        m_cpu->flag_dst = result;
        m_cpu->nzcv_kind = 0;
        m_cpu->rip = 0x1002;
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *jcc_compiled, *m_cpu),
                  JitExecResult::Continue);
        EXPECT_EQ(m_cpu->rip, want_rip) << "result=0x" << std::hex << result;
    }
}
