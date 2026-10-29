// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT execution tests — fused forward and in-trace (internal) Jcc

#include "jit_exec_test_fixture.hpp"

TEST_F(JitExecTest, X64FusedForwardJccStaysInTraceForBothOutcomes) {
    const u8 code[] = {
        0x48, 0x85, 0xC0,                   // test rax,rax
        0x75, 0x05,                         // jnz 0x100A
        0xBB, 0x01, 0x00, 0x00, 0x00,       // mov ebx,1
        0x83, 0xC3, 0x02,                   // add ebx,2
        0xEB, 0x05,                         // jmp 0x1014 (terminal)
    };
    BasicBlock block = build_block64(code, sizeof(code));
    ASSERT_EQ(block.instruction_count, 5u);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    for (const bool taken : {false, true}) {
        SCOPED_TRACE(taken ? "taken" : "untaken");

        CpuState reference;
        reference.init_long_mode();
        reference.rip = 0x1000;
        reference.gpr.set64(EAX, taken ? 1 : 0);
        reference.gpr.set64(EBX, 5);
        Interpreter interp(reference, *m_memory);
        if (taken) {
            ASSERT_EQ(interp.run(2), ExecResult::Branch);
            ASSERT_EQ(reference.rip, 0x100Au);
            ASSERT_EQ(interp.run(2), ExecResult::Branch);
        } else {
            ASSERT_EQ(interp.run(block.instruction_count), ExecResult::Branch);
        }

        m_cpu->init_long_mode();
        m_cpu->rip = 0x1000;
        m_cpu->gpr.set64(EAX, taken ? 1 : 0);
        m_cpu->gpr.set64(EBX, 5);
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, *m_cpu), JitExecResult::Continue);

        EXPECT_EQ(m_cpu->rip, reference.rip);
        EXPECT_EQ(m_cpu->gpr.get64(EBX), reference.gpr.get64(EBX));
        EXPECT_EQ(m_cpu->rip, 0x1014u)
            << "execute returned at the in-block forward target";
        EXPECT_EQ(m_cpu->gpr.get64(EBX), taken ? 7u : 3u);
    }
}

TEST_F(JitExecTest, X64FusedForwardJccTargetFaultHasPreciseRip) {
    const u8 code[] = {
        0x48, 0x85, 0xC0,                   // test rax,rax
        0x75, 0x05,                         // jnz 0x100A
        0xBB, 0x01, 0x00, 0x00, 0x00,       // mov ebx,1 (not reached)
        0x48, 0x8B, 0x19,                   // mov rbx,[rcx] (fault target)
        0xEB, 0x05,                         // jmp 0x1014
    };
    BasicBlock block = build_block64(code, sizeof(code));
    ASSERT_EQ(block.instruction_count, 5u);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    m_cpu->init_long_mode();
    m_cpu->rip = 0x1000;
    m_cpu->gpr.set64(EAX, 1);
    m_cpu->gpr.set64(ECX, 0);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, *m_cpu), JitExecResult::Continue);

    ASSERT_TRUE(m_cpu->pending_raise.active)
        << "taken in-block target returned instead of executing the fault";
    EXPECT_EQ(m_cpu->pending_raise.code, 0xC0000005u);
    EXPECT_EQ(m_cpu->pending_raise.args[1], 0u);
    EXPECT_EQ(m_cpu->rip, 0x100Au);
}

TEST_F(JitExecTest, X64FusedForwardJccFallthroughFaultKeepsPublisherFlags) {
    const u8 code[] = {
        0x48, 0x01, 0xD8,                   // add rax,rbx (publisher)
        0x75, 0x03,                         // jnz 0x1008
        0x48, 0x8B, 0x31,                   // mov rsi,[rcx] (fault)
        0x48, 0x31, 0xD2,                   // xor rdx,rdx (later killer)
        0xEB, 0x05,                         // jmp 0x1012
    };
    BasicBlock block = build_block64(code, sizeof(code));
    ASSERT_EQ(block.instruction_count, 5u);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    auto setup = [](CpuState& cpu) {
        cpu.init_long_mode();
        cpu.rip = 0x1000;
        cpu.gpr.set64(EAX, std::numeric_limits<u64>::max());
        cpu.gpr.set64(EBX, 1);  // result zero: JNZ falls through to the fault
        cpu.gpr.set64(ECX, 0);
        cpu.flags.rflags = 0x2u;  // architectural reserved-one bit only
    };

    CpuState reference;
    setup(reference);
    {
        Interpreter interp(reference, *m_memory);
        ASSERT_EQ(interp.run(block.instruction_count), ExecResult::Exception);
    }
    reference.materialize_lazy_flags();

    setup(*m_cpu);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, *m_cpu), JitExecResult::Continue);
    ASSERT_TRUE(m_cpu->pending_raise.active);
    EXPECT_EQ(m_cpu->pending_raise.code, 0xC0000005u);
    EXPECT_EQ(m_cpu->rip, 0x1005u);
    m_cpu->materialize_lazy_flags();

    EXPECT_EQ(m_cpu->flags.cf(), reference.flags.cf());
    EXPECT_EQ(m_cpu->flags.pf(), reference.flags.pf());
    EXPECT_EQ(m_cpu->flags.af(), reference.flags.af());
    EXPECT_EQ(m_cpu->flags.zf(), reference.flags.zf());
    EXPECT_EQ(m_cpu->flags.sf(), reference.flags.sf());
    EXPECT_EQ(m_cpu->flags.of(), reference.flags.of());
    EXPECT_TRUE(m_cpu->flags.cf());
    EXPECT_TRUE(m_cpu->flags.af());
    EXPECT_TRUE(m_cpu->flags.zf());
}

TEST_F(JitExecTest, X64FusedForwardJccTargetMemoryWriterFaultKeepsFlags) {
    const u8 code[] = {
        0x48, 0x01, 0xD8,                   // add rax,rbx (publisher)
        0x75, 0x05,                         // jnz 0x100A
        0xBE, 0x01, 0x00, 0x00, 0x00,       // mov esi,1 (not reached)
        0x48, 0x83, 0x01, 0x01,             // add qword ptr [rcx],1 (fault)
        0xEB, 0x05,                         // jmp 0x1015
    };
    BasicBlock block = build_block64(code, sizeof(code));
    ASSERT_EQ(block.instruction_count, 5u);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    auto setup = [](CpuState& cpu) {
        cpu.init_long_mode();
        cpu.rip = 0x1000;
        cpu.gpr.set64(EAX, 1);
        cpu.gpr.set64(EBX, 2);  // nonzero: JNZ takes the internal edge
        cpu.gpr.set64(ECX, 0);
        cpu.flags.rflags = 0x2u;  // architectural reserved-one bit only
    };

    CpuState reference;
    setup(reference);
    {
        Interpreter interp(reference, *m_memory);
        ASSERT_EQ(interp.run(2), ExecResult::Branch);
        ASSERT_EQ(reference.rip, 0x100Au);
        ASSERT_EQ(interp.run(1), ExecResult::Exception);
    }
    reference.materialize_lazy_flags();

    setup(*m_cpu);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, *m_cpu), JitExecResult::Continue);
    ASSERT_TRUE(m_cpu->pending_raise.active);
    EXPECT_EQ(m_cpu->pending_raise.code, 0xC0000005u);
    EXPECT_EQ(m_cpu->rip, 0x100Au);
    m_cpu->materialize_lazy_flags();

    EXPECT_EQ(m_cpu->flags.cf(), reference.flags.cf());
    EXPECT_EQ(m_cpu->flags.pf(), reference.flags.pf());
    EXPECT_EQ(m_cpu->flags.af(), reference.flags.af());
    EXPECT_EQ(m_cpu->flags.zf(), reference.flags.zf());
    EXPECT_EQ(m_cpu->flags.sf(), reference.flags.sf());
    EXPECT_EQ(m_cpu->flags.of(), reference.flags.of());
    EXPECT_TRUE(m_cpu->flags.pf());
    EXPECT_FALSE(m_cpu->flags.zf());
}

TEST_F(JitExecTest, MidBlockBackwardJccLoopsInPlace) {
    // MOV EAX,0; INC EAX; CMP EAX,8; JB back-to-INC.
    // The JB target is mid-block, not the block start. Exiting to C++ every
    // iteration is the Sims load-walk tax; this must stay in ARM.
    const u8 code[] = {
        0xB8, 0x00, 0x00, 0x00, 0x00,
        0x40,
        0x83, 0xF8, 0x08,
        0x72, 0xFA,
    };
    m_cpu->gpr.set32(EAX, 0xFFFFFFFFu);
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 8u);
    EXPECT_EQ(m_cpu->rip, 0x1000u + sizeof(code));
}

TEST_F(JitExecTest, MidBlockBackwardJccRejoinsWithArchitecturalCarry) {
    // The first trip into the loop follows XOR (CF=0), while the backedge
    // follows DEC, which preserves the borrow produced by SBB.  A native
    // mid-block rejoin must not reuse the preheader's compile-time carry
    // source on later iterations (Intel SDM Vol. 2 SBB / DEC / Jcc).
    //
    //   xor edx,edx
    //   mov ecx,1
    //   mov eax,0
    // loop:
    //   sbb eax,[ebp+edx*4]
    //   mov [edi+edx*4],eax
    //   dec ecx
    //   mov eax,[esi+edx*4+4]
    //   lea edx,[edx+1]
    //   jge loop
    //   sbb eax,0
    const u8 code[] = {
        0x31, 0xD2,
        0xB9, 0x01, 0x00, 0x00, 0x00,
        0xB8, 0x00, 0x00, 0x00, 0x00,
        0x1B, 0x44, 0x95, 0x00,
        0x89, 0x04, 0x97,
        0x49,
        0x8B, 0x44, 0x96, 0x04,
        0x8D, 0x52, 0x01,
        0x7D, 0xEF,
        0x83, 0xD8, 0x00,
    };

    ASSERT_TRUE(m_memory->write_u32(0x2000, 1));
    ASSERT_TRUE(m_memory->write_u32(0x2004, 0));
    ASSERT_TRUE(m_memory->write_u32(0x4004, 0));
    m_cpu->gpr.set32(EBP, 0x2000);
    m_cpu->gpr.set32(EDI, 0x3000);
    m_cpu->gpr.set32(ESI, 0x4000);

    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_memory->read_u32(0x3000).value(), 0xFFFFFFFFu);
    EXPECT_EQ(m_memory->read_u32(0x3004).value(), 0xFFFFFFFFu);
    EXPECT_EQ(m_cpu->rip, 0x101Du);
    m_cpu->materialize_lazy_flags();
    EXPECT_TRUE(m_cpu->flags.cf());
}


TEST_F(JitExecTest, InternalForwardJccTakenExitsTrace) {
    // cmp eax,0; jz target; mov ebx,1; target: mov ecx,2
    // The block deliberately contains instructions after the Jcc, matching
    // the default fall-through trace shape.
    //
    // The taken edge LEAVES the translation at the target: a 32-bit mid-block
    // forward Jcc compiles to a cold exit, not to the internal B.cond x64
    // takes. Wiring that edge up for 32-bit was measured and is NEUTRAL --
    // see jit-refactor/TRACES32.md.
    const u8 code[] = {
        0x83, 0xF8, 0x00,
        0x74, 0x05,
        0xBB, 0x01, 0x00, 0x00, 0x00,
        0xB9, 0x02, 0x00, 0x00, 0x00,
    };
    m_cpu->gpr.set32(EAX, 0);
    m_cpu->gpr.set32(EBX, 0x11111111);
    m_cpu->gpr.set32(ECX, 0x22222222);

    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EBX), 0x11111111u);
    EXPECT_EQ(m_cpu->gpr.get32(ECX), 0x22222222u);
    EXPECT_EQ(m_cpu->rip, 0x100Au);
    m_cpu->materialize_lazy_flags();
    EXPECT_TRUE(m_cpu->flags.zf());
}

TEST_F(JitExecTest, InternalForwardJccFallthroughStaysInTrace) {
    // Same trace as above, but the condition is false so both following MOVs
    // execute before the trace's ordinary fall-through epilogue.
    const u8 code[] = {
        0x83, 0xF8, 0x00,
        0x74, 0x05,
        0xBB, 0x01, 0x00, 0x00, 0x00,
        0xB9, 0x02, 0x00, 0x00, 0x00,
    };
    m_cpu->gpr.set32(EAX, 7);

    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EBX), 1u);
    EXPECT_EQ(m_cpu->gpr.get32(ECX), 2u);
    EXPECT_EQ(m_cpu->rip, 0x100Fu);
    m_cpu->materialize_lazy_flags();
    EXPECT_FALSE(m_cpu->flags.zf());
}

TEST_F(JitExecTest, TestRm8R8JzMatchesInterpreter) {
    // ContoursCreate: test byte [edi], al / jz …
    // Lazy FlagOp::And8 must drive ZF the same as CMP-based Jcc tests.
    const u8 code[] = {
        0x84, 0x07,       // test [edi], al
        0x74, 0x05,       // jz taken
        0xBB, 0x01, 0x00, 0x00, 0x00,  // mov ebx, 1
        0xB9, 0x02, 0x00, 0x00, 0x00,  // taken: mov ecx, 2
    };
    constexpr GuestAddr cell = 0x9000;

    // Zero cell → ZF=1 → taken exit at target (mov ecx not in this entry).
    ASSERT_TRUE(m_memory->write_u8(cell, 0x00).is_ok());
    m_cpu->gpr.set32(EDI, static_cast<u32>(cell));
    m_cpu->gpr.set32(EAX, 0x01);
    m_cpu->gpr.set32(EBX, 0x11111111);
    m_cpu->gpr.set32(ECX, 0x22222222);
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EBX), 0x11111111u);
    EXPECT_EQ(m_cpu->gpr.get32(ECX), 0x22222222u);
    EXPECT_EQ(m_cpu->rip, 0x1009u);
    m_cpu->materialize_lazy_flags();
    EXPECT_TRUE(m_cpu->flags.zf());

    // Bit set → ZF=0 → fall through both MOVs.
    ASSERT_TRUE(m_memory->write_u8(cell, 0x01).is_ok());
    m_cpu->gpr.set32(EDI, static_cast<u32>(cell));
    m_cpu->gpr.set32(EAX, 0x01);
    m_cpu->gpr.set32(EBX, 0);
    m_cpu->gpr.set32(ECX, 0);
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EBX), 1u);
    EXPECT_EQ(m_cpu->gpr.get32(ECX), 2u);
    m_cpu->materialize_lazy_flags();
    EXPECT_FALSE(m_cpu->flags.zf());
}

TEST_F(JitExecTest, CmpRm8Imm0JgeMatchesInterpreterWithStaleNzcvKind) {
    // ContoursCreate: cmp byte [ecx], 0 / jge …
    // CMP r/m8 publishes zero-extended SUBS NZCV (unsigned-only). A stale
    // nzcv_kind from a prior CMP32 must not let signed JGE fuse that NZCV:
    // 0x80 - 0 as zero-ext has N=0 (JGE would take) but x86 SF=1 (JGE not).
    // Mid-block taken exits leave RIP at the target without running the
    // fall-through MOVs (same shape as TestRm8R8JzMatchesInterpreter).
    const u8 code[] = {
        0x80, 0x39, 0x00,             // cmp byte [ecx], 0
        0x7D, 0x05,                   // jge taken
        0xBB, 0x01, 0x00, 0x00, 0x00, // mov ebx, 1
        0xB9, 0x02, 0x00, 0x00, 0x00, // taken: mov ecx, 2
    };
    constexpr GuestAddr cell = 0x9000;
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    BasicBlock block = build_block(code, sizeof(code));
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    const u8 cells[] = {0x00, 0x01, 0x7F, 0x80, 0xFF};
    for (u8 v : cells) {
        ASSERT_TRUE(m_memory->write_u8(cell, v).is_ok());
        const bool jge_taken = static_cast<i8>(v) >= 0;

        m_cpu->init_protected_mode();
        m_cpu->gpr.set32(ESP, 0x10000);
        m_cpu->gpr.set32(ECX, static_cast<u32>(cell));
        m_cpu->gpr.set32(EBX, 0);
        m_cpu->rip = 0x1000;
        // Poison: predecessor claimed Sub NZCV is live.
        m_cpu->nzcv_kind = 1;
        m_cpu->nzcv_bits = 0;  // N=0,V=0 → GE true if wrongly fused
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, *m_cpu), JitExecResult::Continue)
            << "v=0x" << std::hex << static_cast<unsigned>(v);
        if (jge_taken) {
            EXPECT_EQ(m_cpu->rip, 0x100Au)
                << "v=0x" << std::hex << static_cast<unsigned>(v);
            EXPECT_EQ(m_cpu->gpr.get32(EBX), 0u)
                << "v=0x" << std::hex << static_cast<unsigned>(v);
            EXPECT_EQ(m_cpu->gpr.get32(ECX), static_cast<u32>(cell))
                << "v=0x" << std::hex << static_cast<unsigned>(v);
        } else {
            EXPECT_EQ(m_cpu->rip, 0x100Fu)
                << "v=0x" << std::hex << static_cast<unsigned>(v);
            EXPECT_EQ(m_cpu->gpr.get32(EBX), 1u)
                << "v=0x" << std::hex << static_cast<unsigned>(v);
            EXPECT_EQ(m_cpu->gpr.get32(ECX), 2u)
                << "v=0x" << std::hex << static_cast<unsigned>(v);
        }
    }
}

TEST_F(JitExecTest, InternalForwardJccUsesShr16ResultFlags) {
    // Exact control-flow shape used by the classic Solitaire card renderer:
    //
    //   mov ecx,[esp+4]
    //   xor eax,eax
    //   mov ax,[ecx]
    //   mov dx,ax
    //   shr dx,15
    //   push esi
    //   jz face_down
    //   and eax,0x7fff
    //   mov esi,eax
    //   jmp face_up
    //
    // The forward JZ is internal to the fall-through trace. SHR must replace
    // the prior canonical flags and select the face-up path for each nonzero
    // high-bit result.
    const u8 code[] = {
        0x8B, 0x4C, 0x24, 0x04,
        0x33, 0xC0,
        0x66, 0x8B, 0x01,
        0x66, 0x8B, 0xD0,
        0x66, 0xC1, 0xEA, 0x0F,
        0x56,
        0x74, 0x09,
        0x25, 0xFF, 0x7F, 0x00, 0x00,
        0x8B, 0xF0,
        0xEB, 0x06,
    };
    BasicBlock block = build_block(code, sizeof(code));
    ASSERT_EQ(block.instruction_count, 10u);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    constexpr GuestAddr record = 0x8000;
    const u16 cards[] = {0x8003u, 0x802Au, 0x8040u, 0x8009u};
    for (u16 card : cards) {
        ASSERT_TRUE(m_memory->write_u16(record, card));
        m_cpu->gpr.set32(ESP, 0xF000);
        m_cpu->gpr.set32(ESI, 0xDEADBEEFu);
        // A nonzero SHR result must replace, rather than inherit, an
        // already-materialized zero flag from earlier guest execution.
        m_cpu->flags.set_zf(true);
        m_cpu->rip = 0x1000;
        ASSERT_TRUE(m_memory->write_u32(0xF004, record));

        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, *m_cpu),
                  JitExecResult::Continue);
        EXPECT_EQ(m_cpu->gpr.get32(ESI),
                  static_cast<u32>(card & 0x7FFFu))
            << "card=0x" << std::hex << card;
        EXPECT_EQ(m_cpu->rip, 0x1022u);
    }
}

TEST_F(JitExecTest, WalkLoopAddEsiMatchesInterpreter) {
    // mov ecx,4; add eax,[esi]; add esi,4; sub ecx,1; jnz
    const u8 code[] = {
        0xB9, 0x04, 0x00, 0x00, 0x00,
        0x03, 0x06,
        0x83, 0xC6, 0x04,
        0x83, 0xE9, 0x01,
        0x75, 0xF6,
    };
    ASSERT_TRUE(m_memory->write_u32(0x2000, 1));
    ASSERT_TRUE(m_memory->write_u32(0x2004, 2));
    ASSERT_TRUE(m_memory->write_u32(0x2008, 3));
    ASSERT_TRUE(m_memory->write_u32(0x200C, 4));
    m_cpu->gpr.set32(EAX, 0);
    m_cpu->gpr.set32(ESI, 0x2000);
    m_cpu->gpr.set32(ECX, 99);
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 10u);
    EXPECT_EQ(m_cpu->gpr.get32(ESI), 0x2010u);
    EXPECT_EQ(m_cpu->gpr.get32(ECX), 0u);
}
