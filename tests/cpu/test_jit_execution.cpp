// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT execution tests — verify JIT compiles and executes x86 instructions correctly

#include "jit_exec_test_fixture.hpp"

// ============================================================================
// MOV instructions
// ============================================================================

TEST_F(JitExecTest, MovEaxImm32) {
    // MOV EAX, 0x42 — B8 42000000
    u8 code[] = {0xB8, 0x42, 0x00, 0x00, 0x00};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0x42u);
}

TEST_F(JitExecTest, MovEcxImm32) {
    // MOV ECX, 0xDEADBEEF — B9 EFBEADDE
    u8 code[] = {0xB9, 0xEF, 0xBE, 0xAD, 0xDE};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(ECX), 0xDEADBEEFu);
}

TEST_F(JitExecTest, MovRegReg) {
    m_cpu->gpr.set32(EBX, 0x12345678);
    // MOV EAX, EBX — 8B C3
    u8 code[] = {0x8B, 0xC3};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0x12345678u);
}

TEST_F(JitExecTest, MovMemToReg) {
    // Store value at [0x2000]
    u32 val = 0xCAFEBABE;
    m_memory->write_bytes(0x2000, reinterpret_cast<const u8*>(&val), 4);
    // MOV EAX, [0x2000] — A1 00200000
    u8 code[] = {0xA1, 0x00, 0x20, 0x00, 0x00};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0xCAFEBABEu);
}

TEST_F(JitExecTest, MovRegToMem) {
    m_cpu->gpr.set32(EAX, 0xBAADF00D);
    // MOV [0x2000], EAX — A3 00200000
    u8 code[] = {0xA3, 0x00, 0x20, 0x00, 0x00};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    u32 stored = 0;
    m_memory->read_bytes(0x2000, reinterpret_cast<u8*>(&stored), 4);
    EXPECT_EQ(stored, 0xBAADF00Du);
}

// ============================================================================
// ADD / SUB
// ============================================================================

TEST_F(JitExecTest, AddRegReg) {
    m_cpu->gpr.set32(EAX, 100);
    m_cpu->gpr.set32(EBX, 50);
    // ADD EAX, EBX — 01 D8
    u8 code[] = {0x01, 0xD8};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 150u);
}

TEST_F(JitExecTest, AddRegImm8) {
    m_cpu->gpr.set32(EAX, 100);
    // ADD EAX, 5 — 83 C0 05
    u8 code[] = {0x83, 0xC0, 0x05};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 105u);
}

TEST_F(JitExecTest, SubRegReg) {
    m_cpu->gpr.set32(EAX, 100);
    m_cpu->gpr.set32(EBX, 30);
    // SUB EAX, EBX — 29 D8
    u8 code[] = {0x29, 0xD8};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 70u);
}

TEST_F(JitExecTest, SubRegImm8) {
    m_cpu->gpr.set32(EAX, 100);
    // SUB EAX, 10 — 83 E8 0A
    u8 code[] = {0x83, 0xE8, 0x0A};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 90u);
}

TEST_F(JitExecTest, AddSetsZeroFlag) {
    m_cpu->gpr.set32(EAX, 5);
    m_cpu->gpr.set32(EBX, static_cast<u32>(-5));
    // ADD EAX, EBX — 01 D8
    u8 code[] = {0x01, 0xD8};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0u);
    m_cpu->materialize_lazy_flags();
    EXPECT_TRUE(m_cpu->flags.zf());
}

TEST_F(JitExecTest, AddSetsCarryFlag) {
    m_cpu->gpr.set32(EAX, 0xFFFFFFFF);
    m_cpu->gpr.set32(EBX, 1);
    // ADD EAX, EBX — 01 D8
    u8 code[] = {0x01, 0xD8};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0u);
    m_cpu->materialize_lazy_flags();
    EXPECT_TRUE(m_cpu->flags.cf());
    EXPECT_TRUE(m_cpu->flags.zf());
}

// ============================================================================
// AND / OR / XOR
// ============================================================================

TEST_F(JitExecTest, AndRegReg) {
    m_cpu->gpr.set32(EAX, 0xFF00FF00);
    m_cpu->gpr.set32(EBX, 0x0FF00FF0);
    // AND EAX, EBX — 21 D8
    u8 code[] = {0x21, 0xD8};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0x0F000F00u);
}

TEST_F(JitExecTest, OrRegReg) {
    m_cpu->gpr.set32(EAX, 0xFF00FF00);
    m_cpu->gpr.set32(EBX, 0x00FF00FF);
    // OR EAX, EBX — 09 D8
    u8 code[] = {0x09, 0xD8};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0xFFFFFFFFu);
}

TEST_F(JitExecTest, XorZeroIdiom) {
    m_cpu->gpr.set32(EAX, 0xDEADBEEF);
    // XOR EAX, EAX — 31 C0
    u8 code[] = {0x31, 0xC0};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0u);
    m_cpu->materialize_lazy_flags();
    EXPECT_TRUE(m_cpu->flags.zf());
    EXPECT_FALSE(m_cpu->flags.cf());
}

TEST_F(JitExecTest, XorRegReg) {
    m_cpu->gpr.set32(EAX, 0xAAAAAAAA);
    m_cpu->gpr.set32(EBX, 0x55555555);
    // XOR EAX, EBX — 31 D8
    u8 code[] = {0x31, 0xD8};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0xFFFFFFFFu);
}

// ============================================================================
// CMP / TEST (flag-only, no result stored)
// ============================================================================

TEST_F(JitExecTest, CmpEqual) {
    m_cpu->gpr.set32(EAX, 42);
    m_cpu->gpr.set32(EBX, 42);
    // CMP EAX, EBX — 39 D8
    u8 code[] = {0x39, 0xD8};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    m_cpu->materialize_lazy_flags();
    EXPECT_TRUE(m_cpu->flags.zf());
    EXPECT_FALSE(m_cpu->flags.cf());
}

TEST_F(JitExecTest, CmpLess) {
    m_cpu->gpr.set32(EAX, 10);
    m_cpu->gpr.set32(EBX, 20);
    // CMP EAX, EBX — 39 D8
    u8 code[] = {0x39, 0xD8};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    m_cpu->materialize_lazy_flags();
    EXPECT_FALSE(m_cpu->flags.zf());
    EXPECT_TRUE(m_cpu->flags.cf()); // EAX < EBX → borrow
}

TEST_F(JitExecTest, TestZero) {
    m_cpu->gpr.set32(EAX, 0);
    // TEST EAX, EAX — 85 C0
    u8 code[] = {0x85, 0xC0};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    m_cpu->materialize_lazy_flags();
    EXPECT_TRUE(m_cpu->flags.zf());
}

TEST_F(JitExecTest, TestNonZero) {
    m_cpu->gpr.set32(EAX, 0x80000000);
    // TEST EAX, EAX — 85 C0
    u8 code[] = {0x85, 0xC0};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    m_cpu->materialize_lazy_flags();
    EXPECT_FALSE(m_cpu->flags.zf());
    EXPECT_TRUE(m_cpu->flags.sf()); // MSB set
}

// ============================================================================
// INC / DEC
// ============================================================================

TEST_F(JitExecTest, IncEax) {
    m_cpu->gpr.set32(EAX, 41);
    // INC EAX (32-bit short form) — 40
    u8 code[] = {0x40};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 42u);
}

TEST_F(JitExecTest, DecEax) {
    m_cpu->gpr.set32(EAX, 43);
    // DEC EAX (32-bit short form) — 48
    u8 code[] = {0x48};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 42u);
}

TEST_F(JitExecTest, IncWraps) {
    m_cpu->gpr.set32(EAX, 0xFFFFFFFF);
    // INC EAX — 40
    u8 code[] = {0x40};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0u);
    m_cpu->materialize_lazy_flags();
    EXPECT_TRUE(m_cpu->flags.zf());
}

// ============================================================================
// SHL / SHR / SAR
// ============================================================================

TEST_F(JitExecTest, ShlImm) {
    m_cpu->gpr.set32(EAX, 1);
    // SHL EAX, 4 — C1 E0 04
    u8 code[] = {0xC1, 0xE0, 0x04};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 16u);
}

TEST_F(JitExecTest, ShrImm) {
    m_cpu->gpr.set32(EAX, 0x80);
    // SHR EAX, 4 — C1 E8 04
    u8 code[] = {0xC1, 0xE8, 0x04};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0x8u);
}

TEST_F(JitExecTest, SarPreservesSign) {
    m_cpu->gpr.set32(EAX, 0x80000000);
    // SAR EAX, 4 — C1 F8 04
    u8 code[] = {0xC1, 0xF8, 0x04};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0xF8000000u);
}

// ============================================================================
// NOT / NEG
// ============================================================================

TEST_F(JitExecTest, NotReg) {
    m_cpu->gpr.set32(EAX, 0x00FF00FF);
    // NOT EAX — F7 D0
    u8 code[] = {0xF7, 0xD0};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0xFF00FF00u);
}

TEST_F(JitExecTest, NegReg) {
    m_cpu->gpr.set32(EAX, 42);
    // NEG EAX — F7 D8
    u8 code[] = {0xF7, 0xD8};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), static_cast<u32>(-42));
}

// ============================================================================
// PUSH / POP
// ============================================================================

TEST_F(JitExecTest, PushPop) {
    m_cpu->gpr.set32(EAX, 0xDEADBEEF);
    m_cpu->gpr.set32(EBX, 0);
    u32 orig_esp = m_cpu->gpr.get32(ESP);
    // PUSH EAX; POP EBX — 50 5B
    u8 code[] = {0x50, 0x5B};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EBX), 0xDEADBEEFu);
    EXPECT_EQ(m_cpu->gpr.get32(ESP), orig_esp); // balanced
}

TEST_F(JitExecTest, PushDecrementsEsp) {
    u32 orig_esp = m_cpu->gpr.get32(ESP);
    m_cpu->gpr.set32(EAX, 42);
    // PUSH EAX — 50
    u8 code[] = {0x50};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(ESP), orig_esp - 4);
}

// ============================================================================
// LEA
// ============================================================================

TEST_F(JitExecTest, LeaBaseDisp) {
    m_cpu->gpr.set32(EBX, 0x1000);
    // LEA EAX, [EBX+0x10] — 8D 43 10
    // ModRM 43: mod=01, reg=000(EAX), rm=011(EBX), disp8=0x10
    u8 code[] = {0x8D, 0x43, 0x10};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0x1010u);
}

TEST_F(JitExecTest, LeaBaseIndexScale) {
    m_cpu->gpr.set32(EBX, 0x1000);
    m_cpu->gpr.set32(ECX, 0x10);
    // LEA EAX, [EBX+ECX*4+0x10] — 8D 44 8B 10
    // ModRM 44: mod=01, reg=000(EAX), rm=100(SIB)
    // SIB 8B: scale=10(×4), index=001(ECX), base=011(EBX)
    // disp8=0x10
    u8 code[] = {0x8D, 0x44, 0x8B, 0x10};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0x1000u + 0x10u * 4 + 0x10u);
}

// ============================================================================
// IMUL
// ============================================================================

TEST_F(JitExecTest, ImulRegReg) {
    m_cpu->gpr.set32(EAX, 7);
    m_cpu->gpr.set32(EBX, 6);
    // IMUL EAX, EBX — 0F AF C3
    // ModRM C3: mod=11, reg=000(EAX), rm=011(EBX)
    u8 code[] = {0x0F, 0xAF, 0xC3};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 42u);
}

// ============================================================================
// MOVZX / MOVSX
// ============================================================================

TEST_F(JitExecTest, MovzxR32R8) {
    m_cpu->gpr.set32(EBX, 0xFFFFFF80);
    // MOVZX EAX, BL — 0F B6 C3
    u8 code[] = {0x0F, 0xB6, 0xC3};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0x80u); // zero-extended
}

TEST_F(JitExecTest, MovsxR32R8) {
    m_cpu->gpr.set32(EBX, 0x00000080); // BL = 0x80 (-128)
    // MOVSX EAX, BL — 0F BE C3
    u8 code[] = {0x0F, 0xBE, 0xC3};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0xFFFFFF80u); // sign-extended
}

// ============================================================================
// Multi-instruction blocks
// ============================================================================

TEST_F(JitExecTest, MultiInstrMovAdd) {
    // MOV EAX, 10; ADD EAX, 5 → EAX = 15
    u8 code[] = {
        0xB8, 0x0A, 0x00, 0x00, 0x00,  // MOV EAX, 10
        0x83, 0xC0, 0x05,              // ADD EAX, 5
    };
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 15u);
}

TEST_F(JitExecTest, MultiInstrChain) {
    // MOV EAX, 100; MOV EBX, 200; ADD EAX, EBX; SUB EAX, 50
    u8 code[] = {
        0xB8, 0x64, 0x00, 0x00, 0x00,  // MOV EAX, 100
        0xBB, 0xC8, 0x00, 0x00, 0x00,  // MOV EBX, 200
        0x01, 0xD8,                      // ADD EAX, EBX
        0x83, 0xE8, 0x32,              // SUB EAX, 50
    };
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 250u);
    EXPECT_EQ(m_cpu->gpr.get32(EBX), 200u);
}

TEST_F(JitExecTest, MultiInstrXorAndOr) {
    m_cpu->gpr.set32(EAX, 0xFF);
    m_cpu->gpr.set32(EBX, 0x0F);
    m_cpu->gpr.set32(ECX, 0xF0);
    // XOR EAX, EBX → 0xFF ^ 0x0F = 0xF0
    // AND EAX, ECX → 0xF0 & 0xF0 = 0xF0
    // OR  EAX, EBX → 0xF0 | 0x0F = 0xFF
    u8 code[] = {
        0x31, 0xD8,  // XOR EAX, EBX
        0x21, 0xC8,  // AND EAX, ECX (21 /r: reg=ECX(001), rm=EAX(000) → C8)
        0x09, 0xD8,  // OR  EAX, EBX
    };
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0xFFu);
}

TEST_F(JitExecTest, MultiInstrShiftSequence) {
    m_cpu->gpr.set32(EAX, 1);
    // SHL EAX, 8 → 0x100
    // SHR EAX, 4 → 0x10
    // SHL EAX, 1 → 0x20
    u8 code[] = {
        0xC1, 0xE0, 0x08,  // SHL EAX, 8
        0xC1, 0xE8, 0x04,  // SHR EAX, 4
        0xC1, 0xE0, 0x01,  // SHL EAX, 1
    };
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0x20u);
}

TEST_F(JitExecTest, CmpSibPopJeMatchesInterpreter) {
    // cmp eax, [ecx+edx*4]; pop esi; je +5; mov eax, 0x11
    // Hash-bucket end-iterator check. POP must not drop CMP flags.
    u32 end_iter = 0x11;
    ASSERT_TRUE(m_memory->write_bytes(0x5000,
        reinterpret_cast<const u8*>(&end_iter), 4));
    u32 stack_word = 0xAABBCCDDu;
    ASSERT_TRUE(m_memory->write_bytes(0xFFFC,
        reinterpret_cast<const u8*>(&stack_word), 4));

    const u8 code[] = {
        0x3B, 0x04, 0x91, 0x5E, 0x74, 0x05, 0xB8, 0x11, 0x00, 0x00, 0x00};
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));

    for (u32 eax : {0x11u, 0u, 0xFFFFFFFFu}) {
        CpuState reference;
        reference.init_protected_mode();
        reference.gpr.set32(ESP, 0xFFFC);
        reference.gpr.set32(EAX, eax);
        reference.gpr.set32(ECX, 0x5000);
        reference.gpr.set32(EDX, 0);
        reference.gpr.set32(ESI, 0xDEADBEEFu);
        reference.rip = 0x1000;
        Interpreter interpreter(reference, *m_memory);
        (void)interpreter.run(4);

        m_cpu->init_protected_mode();
        m_cpu->gpr.set32(ESP, 0xFFFC);
        m_cpu->gpr.set32(EAX, eax);
        m_cpu->gpr.set32(ECX, 0x5000);
        m_cpu->gpr.set32(EDX, 0);
        m_cpu->gpr.set32(ESI, 0xDEADBEEFu);
        m_cpu->rip = 0x1000;
        EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue)
            << "eax=0x" << std::hex << eax;
        EXPECT_EQ(m_cpu->gpr.get32(EAX), reference.gpr.get32(EAX))
            << "eax=0x" << std::hex << eax;
        EXPECT_EQ(m_cpu->gpr.get32(ESI), stack_word);
        EXPECT_EQ(m_cpu->gpr.get32(ESP), 0x10000u);
    }
}

TEST_F(JitExecTest, MultiInstrPushPopPreservesAll) {
    m_cpu->gpr.set32(EAX, 0x11111111);
    m_cpu->gpr.set32(EBX, 0x22222222);
    m_cpu->gpr.set32(ECX, 0x33333333);
    u32 orig_esp = m_cpu->gpr.get32(ESP);
    // PUSH EAX; PUSH EBX; PUSH ECX; POP ECX; POP EBX; POP EAX
    u8 code[] = {
        0x50, 0x53, 0x51,  // PUSH EAX, EBX, ECX
        0x59, 0x5B, 0x58,  // POP ECX, EBX, EAX
    };
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0x11111111u);
    EXPECT_EQ(m_cpu->gpr.get32(EBX), 0x22222222u);
    EXPECT_EQ(m_cpu->gpr.get32(ECX), 0x33333333u);
    EXPECT_EQ(m_cpu->gpr.get32(ESP), orig_esp);
}

// ============================================================================
// Memory load/store via register addressing
// ============================================================================

TEST_F(JitExecTest, MovRegFromMem_EbpDisp) {
    // Write a value to [0x5000]
    u32 val = 0x12345678;
    m_memory->write_bytes(0x5000, reinterpret_cast<const u8*>(&val), 4);

    m_cpu->gpr.set32(EBP, 0x5004);
    // MOV EAX, [EBP-4] — 8B 45 FC
    // ModRM 45: mod=01 reg=000(EAX) rm=101(EBP), disp8=FC(-4)
    u8 code[] = {0x8B, 0x45, 0xFC};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0x12345678u);
}

TEST_F(JitExecTest, MovMemFromReg_EbpDisp) {
    m_cpu->gpr.set32(EBP, 0x5004);
    m_cpu->gpr.set32(EAX, 0xABCDEF01);
    // MOV [EBP-4], EAX — 89 45 FC
    u8 code[] = {0x89, 0x45, 0xFC};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    u32 stored = 0;
    m_memory->read_bytes(0x5000, reinterpret_cast<u8*>(&stored), 4);
    EXPECT_EQ(stored, 0xABCDEF01u);
}

// ============================================================================
// EIP advancement
// ============================================================================

TEST_F(JitExecTest, EipAdvancesCorrectly) {
    // NOP; NOP; NOP (3 single-byte instructions → EIP should advance by 3)
    u8 code[] = {0x90, 0x90, 0x90};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(static_cast<u32>(m_cpu->rip), 0x1003u);
}

TEST_F(JitExecTest, PauseAdvancesRip) {
    // F3 90 — PAUSE is a spin-wait hint with no architectural state change
    // (Intel SDM Vol. 2 PAUSE). Same guest-visible effect as NOP.
    const u64 flags_before = m_cpu->flags.rflags;
    const u32 eax_before = m_cpu->gpr.get32(EAX);
    u8 code[] = {0xF3, 0x90};
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(static_cast<u32>(m_cpu->rip), 0x1002u);
    EXPECT_EQ(m_cpu->flags.rflags, flags_before);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), eax_before);
}

TEST_F(JitExecTest, EipAdvancesMultiInstMixed) {
    // MOV EAX, 0(5B) + ADD EAX,1(3B) + NOP(1B) = 9 bytes
    u8 code[] = {
        0xB8, 0x00, 0x00, 0x00, 0x00,  // MOV EAX, 0
        0x83, 0xC0, 0x01,              // ADD EAX, 1
        0x90,                           // NOP
    };
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(static_cast<u32>(m_cpu->rip), 0x1009u);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 1u);
}
