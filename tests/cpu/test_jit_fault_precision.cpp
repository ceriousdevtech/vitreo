// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT execution tests — fault precision and deferred x87 exception delivery

#include "jit_exec_test_fixture.hpp"

TEST_F(JitExecTest, FstpM32WriteFaultDoesNotPopX87Stack) {
    m_cpu->fpu.push(Float80(7.5));
    const u8 top_before = m_cpu->fpu.top();
    const u16 tag_before = m_cpu->fpu.tag_word;
    const Float80 value_before = m_cpu->fpu.get_st(0);

    const u8 code[] = {
        0xD9, 0x1D, 0x00, 0x00, 0x00, 0x20, // fstp dword ptr [0x20000000]
    };

    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    ASSERT_TRUE(m_cpu->pending_raise.active);
    EXPECT_EQ(m_cpu->pending_raise.code, 0xC0000005u);
    EXPECT_EQ(m_cpu->fpu.top(), top_before);
    EXPECT_EQ(m_cpu->fpu.tag_word, tag_before);
    EXPECT_EQ(m_cpu->fpu.get_st(0).mantissa, value_before.mantissa);
    EXPECT_EQ(m_cpu->fpu.get_st(0).exponent_sign,
              value_before.exponent_sign);
}

TEST_F(JitExecTest, FistpAndFisttpSuppressUnmaskedPrecisionCommit) {
    constexpr u32 kDestination = 0x2400;
    constexpr u32 kSentinel = 0xA5A55A5Au;
    const auto run = [&](const std::array<u8, 6>& code, u16 expected_status) {
        m_cpu->fpu.reset();
        m_cpu->fpu.control_word = 0x035F; // precision unmasked
        m_cpu->fpu.push(Float80(1.5));
        m_cpu->rip = 0x1000;
        ASSERT_TRUE(m_memory->write_u32(kDestination, kSentinel));
        const u8 top = m_cpu->fpu.top();
        const u16 tags = m_cpu->fpu.tag_word;

        EXPECT_EQ(jit_run(code.data(), code.size()), JitExecResult::Continue);
        ASSERT_TRUE(m_memory->read_u32(kDestination).is_ok());
        EXPECT_EQ(m_memory->read_u32(kDestination).value(), kSentinel);
        EXPECT_EQ(m_cpu->fpu.top(), top);
        EXPECT_EQ(m_cpu->fpu.tag_word, tags);
        EXPECT_EQ(m_cpu->fpu.status_word, expected_status);
    };

    run({0xDB, 0x1D, 0x00, 0x24, 0x00, 0x00}, // FISTP m32 [0x2400]
        static_cast<u16>(0x3800u | FPU_SW_B | FPU_SW_ES |
                         FPU_SW_C1 | FPU_SW_PE));
    run({0xDB, 0x0D, 0x00, 0x24, 0x00, 0x00}, // FISTTP m32 [0x2400]
        static_cast<u16>(0x3800u | FPU_SW_B | FPU_SW_ES | FPU_SW_PE));
}

TEST_F(JitExecTest, FldM32ReadFaultDoesNotPushX87Stack) {
    const u8 top_before = m_cpu->fpu.top();
    const u16 tag_before = m_cpu->fpu.tag_word;
    const u8 code[] = {
        0xD9, 0x05, 0x00, 0x00, 0x00, 0x20, // fld dword ptr [0x20000000]
    };

    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    ASSERT_TRUE(m_cpu->pending_raise.active);
    EXPECT_EQ(m_cpu->pending_raise.code, 0xC0000005u);
    EXPECT_EQ(m_cpu->fpu.top(), top_before);
    EXPECT_EQ(m_cpu->fpu.tag_word, tag_before);
}

TEST_F(JitExecTest, FaddM32ReadFaultDoesNotChangeX87Stack) {
    m_cpu->fpu.push(Float80(7.5));
    const u8 top_before = m_cpu->fpu.top();
    const u16 tag_before = m_cpu->fpu.tag_word;
    const Float80 value_before = m_cpu->fpu.get_st(0);
    const u8 code[] = {
        0xD8, 0x05, 0x00, 0x00, 0x00, 0x20, // fadd dword ptr [0x20000000]
    };

    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    ASSERT_TRUE(m_cpu->pending_raise.active);
    EXPECT_EQ(m_cpu->pending_raise.code, 0xC0000005u);
    EXPECT_EQ(m_cpu->fpu.top(), top_before);
    EXPECT_EQ(m_cpu->fpu.tag_word, tag_before);
    EXPECT_EQ(m_cpu->fpu.get_st(0).mantissa, value_before.mantissa);
    EXPECT_EQ(m_cpu->fpu.get_st(0).exponent_sign,
              value_before.exponent_sign);
}

TEST_F(JitExecTest, X64PendingX87DeliveryPreservesCmpFlagsOnRaiseExit) {
    // Intel SDM Vol. 1 section 8.3.12: a waiting x87 instruction delivers a
    // deferred unmasked exception before its primary operation. FWAIT and the
    // delivered exception do not redefine integer EFLAGS. Windows reports
    // PF|ZF for this equal CMP (RF is exception-context metadata, not an
    // arithmetic flag).
    const u8 code[] = {
        0x39, 0xD1, // cmp ecx,edx
        0x9B,       // fwait
    };
    auto setup = [](CpuState& cpu) {
        cpu.init_long_mode();
        cpu.rip = 0x1000;
        cpu.gpr.set32(ECX, 5);
        cpu.gpr.set32(EDX, 5);
        cpu.flags.rflags = 0x2;
        cpu.fpu.control_word = 0x037B; // divide-by-zero unmasked
        cpu.fpu.status_word = FPU_SW_ES | FPU_SW_ZE;
    };

    CpuState reference;
    setup(reference);
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    {
        Interpreter interp(reference, *m_memory);
        EXPECT_EQ(interp.run(2), ExecResult::Exception);
    }
    reference.materialize_lazy_flags();

    setup(*m_cpu);
    EXPECT_EQ(jit_run64(code, sizeof(code)), JitExecResult::Continue);
    ASSERT_TRUE(m_cpu->pending_raise.active);
    EXPECT_EQ(m_cpu->pending_raise.code, 0xC000008Eu);
    EXPECT_EQ(m_cpu->rip, 0x1002u);
    ASSERT_NE(m_cpu->nzcv_kind, 0u);
    EXPECT_EQ(m_cpu->nzcv_bits & 0xF0000000u, 0x60000000u);
    m_cpu->materialize_lazy_flags();

    constexpr u64 arithmetic = FLAG_CF | FLAG_PF | FLAG_AF |
                               FLAG_ZF | FLAG_SF | FLAG_OF;
    EXPECT_EQ(m_cpu->flags.rflags & arithmetic,
              reference.flags.rflags & arithmetic);
    EXPECT_EQ(m_cpu->flags.rflags & arithmetic, FLAG_PF | FLAG_ZF);
}

TEST_F(JitExecTest, X64UnmaskedX87OverflowCommitsScaledStBeforeDeferredFault) {
    const u8 code[] = {
        0xD8, 0xC9, // fmul st(0),st(1)
        0xDF, 0xE0, // fnstsw ax -- observes pending state without waiting
        0x9B,       // fwait -- delivers #O
    };
    auto setup = [](CpuState& cpu) {
        cpu.init_long_mode();
        cpu.rip = 0x1000;
        cpu.fpu.control_word = 0x0377; // PC=64, nearest, #O unmasked
        cpu.fpu.status_word = 0;
        cpu.fpu.tag_word = 0xFFF0;     // physical ST0/ST1 valid
        cpu.fpu.st[0] = Float80::from_extended(
            0xFFFFFFFFFFFFFFFFull, 0x7FFEu);
        cpu.fpu.st[1] = Float80(2.0);
    };

    CpuState reference;
    setup(reference);
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    {
        Interpreter interp(reference, *m_memory);
        EXPECT_EQ(interp.run(3), ExecResult::Exception);
    }
    EXPECT_EQ(reference.rip, 0x1004u);
    EXPECT_EQ(reference.gpr.get16(EAX), 0x8088u);
    EXPECT_EQ(reference.fpu.get_st(0).mantissa, 0xFFFFFFFFFFFFFFFFull);
    EXPECT_EQ(reference.fpu.get_st(0).exponent_sign, 0x1FFFu);

    setup(*m_cpu);
    EXPECT_EQ(jit_run64(code, sizeof(code)), JitExecResult::Continue);
    ASSERT_TRUE(m_cpu->pending_raise.active);
    EXPECT_EQ(m_cpu->pending_raise.code, 0xC0000091u);
    EXPECT_EQ(m_cpu->rip, 0x1004u);
    EXPECT_EQ(m_cpu->gpr.get16(EAX), reference.gpr.get16(EAX));
    EXPECT_EQ(m_cpu->fpu.status_word, reference.fpu.status_word);
    EXPECT_EQ(m_cpu->fpu.get_st(0).mantissa,
              reference.fpu.get_st(0).mantissa);
    EXPECT_EQ(m_cpu->fpu.get_st(0).exponent_sign,
              reference.fpu.get_st(0).exponent_sign);
}

TEST_F(JitExecTest, X64FldcwUnmaskDeliversStickyExceptionAtNextWait) {
    constexpr u64 kControl = 0x3000;
    const u8 code[] = {
        0x39, 0xD1, // cmp ecx,edx
        0xD9, 0x2B, // fldcw word ptr [rbx]
        0xDF, 0xE0, // fnstsw ax (non-waiting)
        0x9B,       // fwait
    };
    auto setup = [&](CpuState& cpu) {
        cpu.init_long_mode();
        cpu.rip = 0x1000;
        cpu.gpr.set32(ECX, 5);
        cpu.gpr.set32(EDX, 5);
        cpu.gpr.set64(EBX, kControl);
        cpu.flags.rflags = 0x2;
        cpu.fpu.control_word = 0x037F;
        cpu.fpu.status_word = FPU_SW_ZE;
    };
    ASSERT_TRUE(m_memory->write_u16(kControl, 0x037B).is_ok());

    CpuState reference;
    setup(reference);
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    {
        Interpreter interp(reference, *m_memory);
        EXPECT_EQ(interp.run(4), ExecResult::Exception);
    }
    EXPECT_EQ(reference.rip, 0x1006u);
    EXPECT_EQ(reference.gpr.get16(EAX), 0x8084u);

    setup(*m_cpu);
    EXPECT_EQ(jit_run64(code, sizeof(code)), JitExecResult::Continue);
    ASSERT_TRUE(m_cpu->pending_raise.active);
    EXPECT_EQ(m_cpu->pending_raise.code, 0xC000008Eu);
    EXPECT_EQ(m_cpu->rip, 0x1006u);
    EXPECT_EQ(m_cpu->gpr.get16(EAX), 0x8084u);
    EXPECT_EQ(m_cpu->fpu.control_word, 0x037Bu);
    EXPECT_EQ(m_cpu->fpu.status_word, 0x8084u);
    m_cpu->materialize_lazy_flags();
    EXPECT_EQ(m_cpu->flags.rflags & 0x8D5u, FLAG_PF | FLAG_ZF);
}

TEST_F(JitExecTest, X64FcomMemoryFaultPreservesCmpFlagsOnRaiseExit) {
    // FCOM changes only x87 C3/C2/C1/C0. A checked source-memory fault occurs
    // before that update and must also leave a preceding CMP's EFLAGS intact.
    const u8 code[] = {
        0x39, 0xD1, // cmp ecx,edx
        0xD8, 0x10, // fcom dword ptr [rax]
    };
    auto setup = [](CpuState& cpu) {
        cpu.init_long_mode();
        cpu.rip = 0x1000;
        cpu.gpr.set32(ECX, 5);
        cpu.gpr.set32(EDX, 5);
        cpu.gpr.set64(EAX, 0x20000000u); // unmapped
        cpu.flags.rflags = 0x2;
        cpu.fpu.push(Float80(1.0));
    };

    CpuState reference;
    setup(reference);
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    {
        Interpreter interp(reference, *m_memory);
        EXPECT_EQ(interp.run(2), ExecResult::Exception);
    }
    reference.materialize_lazy_flags();

    setup(*m_cpu);
    EXPECT_EQ(jit_run64(code, sizeof(code)), JitExecResult::Continue);
    ASSERT_TRUE(m_cpu->pending_raise.active);
    EXPECT_EQ(m_cpu->pending_raise.code, 0xC0000005u);
    EXPECT_EQ(m_cpu->rip, 0x1002u);
    ASSERT_NE(m_cpu->nzcv_kind, 0u);
    EXPECT_EQ(m_cpu->nzcv_bits & 0xF0000000u, 0x60000000u);
    m_cpu->materialize_lazy_flags();

    constexpr u64 arithmetic = FLAG_CF | FLAG_PF | FLAG_AF |
                               FLAG_ZF | FLAG_SF | FLAG_OF;
    EXPECT_EQ(m_cpu->flags.rflags & arithmetic,
              reference.flags.rflags & arithmetic);
    EXPECT_EQ(m_cpu->flags.rflags & arithmetic, FLAG_PF | FLAG_ZF);
}

TEST_F(JitExecTest, DelegatedSseReadFaultRaisesAccessViolation) {
    const u8 code[] = {0x66, 0x0F, 0x75, 0x02}; // pcmpeqw xmm0, [edx]
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    m_cpu->gpr.set32(EDX, 0x20000000);

    jit_interpret_step(m_cpu.get(), m_memory.get(), 0x1000);

    ASSERT_TRUE(m_cpu->pending_raise.active);
    EXPECT_TRUE(m_cpu->pending_raise.from_helper);
    EXPECT_EQ(m_cpu->pending_raise.code, 0xC0000005u);
    EXPECT_EQ(m_cpu->pending_raise.num_args, 2u);
    EXPECT_EQ(m_cpu->pending_raise.args[0], 0u);
    EXPECT_EQ(m_cpu->pending_raise.args[1], 0x20000000u);
}

TEST_F(JitExecTest, DelegatedInt3StopsAtPostBreakpointResumeAddress) {
    const u8 code[] = {
        0xCC,                         // int3
        0xB8, 0x2A, 0x00, 0x00, 0x00 // mov eax,42 (must not execute yet)
    };
    m_cpu->gpr.set32(EAX, 0);

    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);

    ASSERT_TRUE(m_cpu->pending_raise.active);
    EXPECT_TRUE(m_cpu->pending_raise.from_helper);
    EXPECT_EQ(m_cpu->pending_raise.code, 0x80000003u);
    EXPECT_EQ(m_cpu->rip, 0x1001u);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0u);
}

TEST_F(JitExecTest, X64NativeReadFaultPreservesInstructionRip) {
    const u8 code[] = {
        0x48, 0xB8, 0x2A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x48, 0x8B, 0x01,
    }; // mov rax, 42; mov rax, [rcx]
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));

    m_cpu->mode = CpuMode::Long;
    m_cpu->rip = 0x1000;
    m_cpu->gpr.rcx = 0;

    BasicBlock block;
    block.start_addr = 0x1000;
    block.is_64bit = true;
    MemoryByteReader reader(code, sizeof(code));
    while (reader.remaining() > 0) {
        DecodedInstruction instruction;
        ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode64, instruction),
                  DecodeError::Success);
        block.instructions.push_back(instruction);
    }
    block.byte_length = static_cast<u32>(reader.position());
    block.instruction_count = static_cast<u32>(block.instructions.size());

    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, *m_cpu), JitExecResult::Continue);

    ASSERT_TRUE(m_cpu->pending_raise.active);
    EXPECT_EQ(m_cpu->pending_raise.code, 0xC0000005u);
    EXPECT_EQ(m_cpu->pending_raise.args[1], 0u);
    EXPECT_EQ(m_cpu->rip, 0x100Au);
}
