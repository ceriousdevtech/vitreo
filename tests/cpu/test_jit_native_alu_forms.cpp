// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT execution tests — native NOT/MUL/DIV/IDIV, MOFFS and byte ALU forms

#include "jit_exec_test_fixture.hpp"

TEST_F(JitExecTest, NotByteRegisterAndMemoryAreNativeAndPreserveFlags) {
    struct DelegateGuard {
        DelegateGuard() { setenv("VITREO_JIT_NO_DELEGATE", "1", 1); }
        ~DelegateGuard() { unsetenv("VITREO_JIT_NO_DELEGATE"); }
    } delegate_guard;

    m_cpu->gpr.set32(ECX, 0x12345678u);
    m_cpu->flags.rflags = 0xCD7u;
    m_cpu->flag_op = CpuState::FlagOp::None;
    ASSERT_TRUE(m_memory->write_u8(0x2000, 0xA5u));

    const u8 code[] = {
        0xF6, 0xD1,                         // not cl
        0xF6, 0x15, 0x00, 0x20, 0x00, 0x00, // not byte ptr [0x2000]
    };
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(ECX), 0x12345687u);
    ASSERT_TRUE(m_memory->read_u8(0x2000).is_ok());
    EXPECT_EQ(m_memory->read_u8(0x2000).value(), 0x5Au);
    EXPECT_EQ(m_cpu->flags.rflags, 0xCD7u);
    EXPECT_EQ(m_cpu->flag_op, CpuState::FlagOp::None);
}

TEST_F(JitExecTest, DivRm32RegisterAndMemoryAreNative) {
    struct DelegateGuard {
        DelegateGuard() { setenv("VITREO_JIT_NO_DELEGATE", "1", 1); }
        ~DelegateGuard() { unsetenv("VITREO_JIT_NO_DELEGATE"); }
    } delegate_guard;

    // EDX:EAX = 0x00000001'00000000; / 3 = 0x55555555 remainder 1.
    m_cpu->gpr.set32(EAX, 0);
    m_cpu->gpr.set32(EDX, 1);
    m_cpu->gpr.set32(ECX, 3);
    const u8 register_code[] = {0xF7, 0xF1}; // div ecx
    EXPECT_EQ(jit_run(register_code, sizeof(register_code)),
              JitExecResult::Continue);
    EXPECT_FALSE(m_cpu->pending_raise.active);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0x55555555u);
    EXPECT_EQ(m_cpu->gpr.get32(EDX), 1u);

    m_jit = std::make_unique<JitCompiler>(*m_memory);
    m_cpu->rip = 0x1000;
    m_cpu->gpr.set32(EAX, 0xFFFFFFFFu);
    m_cpu->gpr.set32(EDX, 0);
    ASSERT_TRUE(m_memory->write_u32(0x2000, 10));
    const u8 memory_code[] = {
        0xF7, 0x35, 0x00, 0x20, 0x00, 0x00 // div dword ptr [0x2000]
    };
    EXPECT_EQ(jit_run(memory_code, sizeof(memory_code)),
              JitExecResult::Continue);
    EXPECT_FALSE(m_cpu->pending_raise.active);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0x19999999u);
    EXPECT_EQ(m_cpu->gpr.get32(EDX), 5u);
}

TEST_F(JitExecTest, DivRm32NativeRaisesDivideErrorWithoutCommitting) {
    struct DelegateGuard {
        DelegateGuard() { setenv("VITREO_JIT_NO_DELEGATE", "1", 1); }
        ~DelegateGuard() { unsetenv("VITREO_JIT_NO_DELEGATE"); }
    } delegate_guard;

    const u8 code[] = {0xF7, 0xF1}; // div ecx
    m_cpu->gpr.set32(EAX, 0x12345678u);
    m_cpu->gpr.set32(EDX, 0x9ABCDEF0u);
    m_cpu->gpr.set32(ECX, 0); // zero divisor
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    ASSERT_TRUE(m_cpu->pending_raise.active);
    EXPECT_TRUE(m_cpu->pending_raise.from_helper);
    EXPECT_EQ(m_cpu->pending_raise.code, 0xC0000094u);
    EXPECT_EQ(m_cpu->rip, 0x1000u);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0x12345678u);
    EXPECT_EQ(m_cpu->gpr.get32(EDX), 0x9ABCDEF0u);

    m_jit = std::make_unique<JitCompiler>(*m_memory);
    m_cpu->pending_raise.clear();
    m_cpu->rip = 0x1000;
    m_cpu->gpr.set32(EAX, 7);
    m_cpu->gpr.set32(EDX, 5);
    m_cpu->gpr.set32(ECX, 5); // EDX >= divisor => quotient overflow
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    ASSERT_TRUE(m_cpu->pending_raise.active);
    EXPECT_EQ(m_cpu->pending_raise.code, 0xC0000094u);
    EXPECT_EQ(m_cpu->rip, 0x1000u);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 7u);
    EXPECT_EQ(m_cpu->gpr.get32(EDX), 5u);
}

TEST_F(JitExecTest, IdivRm32IsNativeAndExceptionCorrect) {
    struct DelegateGuard {
        DelegateGuard() { setenv("VITREO_JIT_NO_DELEGATE", "1", 1); }
        ~DelegateGuard() { unsetenv("VITREO_JIT_NO_DELEGATE"); }
    } delegate_guard;

    const u8 code[] = {0xF7, 0xF9}; // idiv ecx
    m_cpu->gpr.set32(EAX, 0xFFFFFFF9u); // EDX:EAX = -7
    m_cpu->gpr.set32(EDX, 0xFFFFFFFFu);
    m_cpu->gpr.set32(ECX, 3);
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_FALSE(m_cpu->pending_raise.active);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0xFFFFFFFEu); // -2
    EXPECT_EQ(m_cpu->gpr.get32(EDX), 0xFFFFFFFFu); // -1

    m_jit = std::make_unique<JitCompiler>(*m_memory);
    m_cpu->pending_raise.clear();
    m_cpu->rip = 0x1000;
    m_cpu->gpr.set32(EAX, 0x80000000u); // -2147483648
    m_cpu->gpr.set32(EDX, 0xFFFFFFFFu);
    m_cpu->gpr.set32(ECX, 0xFFFFFFFFu); // / -1 overflows signed dword
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    ASSERT_TRUE(m_cpu->pending_raise.active);
    EXPECT_EQ(m_cpu->pending_raise.code, 0xC0000094u);
    EXPECT_EQ(m_cpu->rip, 0x1000u);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0x80000000u);
    EXPECT_EQ(m_cpu->gpr.get32(EDX), 0xFFFFFFFFu);
}

TEST_F(JitExecTest, MulImulDivIdivRm8AreNative) {
    struct DelegateGuard {
        DelegateGuard() { setenv("VITREO_JIT_NO_DELEGATE", "1", 1); }
        ~DelegateGuard() { unsetenv("VITREO_JIT_NO_DELEGATE"); }
    } delegate_guard;

    // MUL CL: AL=5 * CL=3 → AX=15, CF=OF=0
    m_cpu->gpr.set32(EAX, 0x12345605u);
    m_cpu->gpr.set32(ECX, 3);
    const u8 mul_code[] = {0xF6, 0xE1}; // mul cl
    EXPECT_EQ(jit_run(mul_code, sizeof(mul_code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get16(EAX), 15u);
    jit_materialize_flags(m_cpu.get());
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.of());

    m_jit = std::make_unique<JitCompiler>(*m_memory);
    m_cpu->rip = 0x1000;
    // IMUL CL: AL=-3 * CL=7 → AX=-21, fits in AL → CF=OF=0
    m_cpu->gpr.set32(EAX, 0xABCDEF00u | static_cast<u8>(static_cast<i8>(-3)));
    m_cpu->gpr.set32(ECX, 7);
    const u8 imul_code[] = {0xF6, 0xE9}; // imul cl
    EXPECT_EQ(jit_run(imul_code, sizeof(imul_code)), JitExecResult::Continue);
    EXPECT_EQ(static_cast<i16>(m_cpu->gpr.get16(EAX)), static_cast<i16>(-21));
    jit_materialize_flags(m_cpu.get());
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.of());

    m_jit = std::make_unique<JitCompiler>(*m_memory);
    m_cpu->rip = 0x1000;
    // DIV CL: AX=100 / CL=7 → AL=14 rem AH=2
    m_cpu->gpr.set32(EAX, 100);
    m_cpu->gpr.set32(ECX, 7);
    const u8 div_code[] = {0xF6, 0xF1}; // div cl
    EXPECT_EQ(jit_run(div_code, sizeof(div_code)), JitExecResult::Continue);
    EXPECT_FALSE(m_cpu->pending_raise.active);
    EXPECT_EQ(m_cpu->gpr.get8l(EAX), 14u);
    EXPECT_EQ(m_cpu->gpr.get8h(0), 2u);

    m_jit = std::make_unique<JitCompiler>(*m_memory);
    m_cpu->rip = 0x1000;
    // IDIV CL: AX=-20 / CL=3 → AL=-6 rem AH=-2
    m_cpu->gpr.set32(EAX, static_cast<u16>(static_cast<i16>(-20)));
    m_cpu->gpr.set32(ECX, 3);
    const u8 idiv_code[] = {0xF6, 0xF9}; // idiv cl
    EXPECT_EQ(jit_run(idiv_code, sizeof(idiv_code)), JitExecResult::Continue);
    EXPECT_FALSE(m_cpu->pending_raise.active);
    EXPECT_EQ(static_cast<i8>(m_cpu->gpr.get8l(EAX)), static_cast<i8>(-6));
    EXPECT_EQ(static_cast<i8>(m_cpu->gpr.get8h(0)), static_cast<i8>(-2));
}

TEST_F(JitExecTest, DivRm8NativeRaisesDivideErrorWithoutCommitting) {
    struct DelegateGuard {
        DelegateGuard() { setenv("VITREO_JIT_NO_DELEGATE", "1", 1); }
        ~DelegateGuard() { unsetenv("VITREO_JIT_NO_DELEGATE"); }
    } delegate_guard;

    const u8 code[] = {0xF6, 0xF1}; // div cl
    m_cpu->gpr.set32(EAX, 0x1234u);
    m_cpu->gpr.set32(ECX, 0); // zero divisor
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    ASSERT_TRUE(m_cpu->pending_raise.active);
    EXPECT_TRUE(m_cpu->pending_raise.from_helper);
    EXPECT_EQ(m_cpu->pending_raise.code, 0xC0000094u);
    EXPECT_EQ(m_cpu->rip, 0x1000u);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0x1234u);

    m_jit = std::make_unique<JitCompiler>(*m_memory);
    m_cpu->pending_raise.clear();
    m_cpu->rip = 0x1000;
    // AX=0x100 / CL=1 → quotient 256 does not fit in AL
    m_cpu->gpr.set32(EAX, 0x100u);
    m_cpu->gpr.set32(ECX, 1);
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    ASSERT_TRUE(m_cpu->pending_raise.active);
    EXPECT_EQ(m_cpu->pending_raise.code, 0xC0000094u);
    EXPECT_EQ(m_cpu->rip, 0x1000u);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0x100u);
}

TEST_F(JitExecTest, MulImulDivIdivRm16AreNative) {
    struct DelegateGuard {
        DelegateGuard() { setenv("VITREO_JIT_NO_DELEGATE", "1", 1); }
        ~DelegateGuard() { unsetenv("VITREO_JIT_NO_DELEGATE"); }
    } delegate_guard;

    // MUL CX: AX=5 * CX=3 → DX:AX=15, CF=OF=0; upper EAX/EDX preserved.
    m_cpu->gpr.set32(EAX, 0x12340005u);
    m_cpu->gpr.set32(EDX, 0xABCD9999u);
    m_cpu->gpr.set32(ECX, 3);
    const u8 mul_code[] = {0x66, 0xF7, 0xE1}; // mul cx
    EXPECT_EQ(jit_run(mul_code, sizeof(mul_code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get16(EAX), 15u);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0x1234000Fu);
    EXPECT_EQ(m_cpu->gpr.get16(EDX), 0u);
    EXPECT_EQ(m_cpu->gpr.get32(EDX), 0xABCD0000u);
    jit_materialize_flags(m_cpu.get());
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.of());

    m_jit = std::make_unique<JitCompiler>(*m_memory);
    m_cpu->rip = 0x1000;
    // IMUL CX: AX=-3 * CX=7 → DX:AX=-21, fits in AX → CF=OF=0
    m_cpu->gpr.set32(EAX, 0x5555FFFDu);
    m_cpu->gpr.set32(EDX, 0x66667777u);
    m_cpu->gpr.set32(ECX, 7);
    const u8 imul_code[] = {0x66, 0xF7, 0xE9}; // imul cx
    EXPECT_EQ(jit_run(imul_code, sizeof(imul_code)), JitExecResult::Continue);
    EXPECT_EQ(static_cast<i16>(m_cpu->gpr.get16(EAX)), static_cast<i16>(-21));
    EXPECT_EQ(m_cpu->gpr.get16(EDX), 0xFFFFu);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0x5555FFEBu);
    EXPECT_EQ(m_cpu->gpr.get32(EDX), 0x6666FFFFu);
    jit_materialize_flags(m_cpu.get());
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.of());

    m_jit = std::make_unique<JitCompiler>(*m_memory);
    m_cpu->rip = 0x1000;
    // DIV CX: DX:AX=1000 / CX=7 → AX=142 rem DX=6
    m_cpu->gpr.set32(EAX, 0xAAAA03E8u);
    m_cpu->gpr.set32(EDX, 0xBBBB0000u);
    m_cpu->gpr.set32(ECX, 7);
    const u8 div_code[] = {0x66, 0xF7, 0xF1}; // div cx
    EXPECT_EQ(jit_run(div_code, sizeof(div_code)), JitExecResult::Continue);
    EXPECT_FALSE(m_cpu->pending_raise.active);
    EXPECT_EQ(m_cpu->gpr.get16(EAX), 142u);
    EXPECT_EQ(m_cpu->gpr.get16(EDX), 6u);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0xAAAA008Eu);
    EXPECT_EQ(m_cpu->gpr.get32(EDX), 0xBBBB0006u);

    m_jit = std::make_unique<JitCompiler>(*m_memory);
    m_cpu->rip = 0x1000;
    // IDIV CX: DX:AX=-20 / CX=3 → AX=-6 rem DX=-2
    m_cpu->gpr.set32(EAX, 0xCCCCFFECu);
    m_cpu->gpr.set32(EDX, 0xDDDDFFFFu);
    m_cpu->gpr.set32(ECX, 3);
    const u8 idiv_code[] = {0x66, 0xF7, 0xF9}; // idiv cx
    EXPECT_EQ(jit_run(idiv_code, sizeof(idiv_code)), JitExecResult::Continue);
    EXPECT_FALSE(m_cpu->pending_raise.active);
    EXPECT_EQ(static_cast<i16>(m_cpu->gpr.get16(EAX)), static_cast<i16>(-6));
    EXPECT_EQ(static_cast<i16>(m_cpu->gpr.get16(EDX)), static_cast<i16>(-2));
}

TEST_F(JitExecTest, DivRm16NativeRaisesDivideErrorWithoutCommitting) {
    struct DelegateGuard {
        DelegateGuard() { setenv("VITREO_JIT_NO_DELEGATE", "1", 1); }
        ~DelegateGuard() { unsetenv("VITREO_JIT_NO_DELEGATE"); }
    } delegate_guard;

    const u8 code[] = {0x66, 0xF7, 0xF1}; // div cx
    m_cpu->gpr.set32(EAX, 0x12345678u);
    m_cpu->gpr.set32(EDX, 0x9ABCDEADu);
    m_cpu->gpr.set32(ECX, 0); // zero divisor
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    ASSERT_TRUE(m_cpu->pending_raise.active);
    EXPECT_TRUE(m_cpu->pending_raise.from_helper);
    EXPECT_EQ(m_cpu->pending_raise.code, 0xC0000094u);
    EXPECT_EQ(m_cpu->rip, 0x1000u);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0x12345678u);
    EXPECT_EQ(m_cpu->gpr.get32(EDX), 0x9ABCDEADu);

    m_jit = std::make_unique<JitCompiler>(*m_memory);
    m_cpu->pending_raise.clear();
    m_cpu->rip = 0x1000;
    // DX >= CX → quotient overflow (DX:AX / CX)
    m_cpu->gpr.set32(EAX, 7);
    m_cpu->gpr.set32(EDX, 5);
    m_cpu->gpr.set32(ECX, 5);
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    ASSERT_TRUE(m_cpu->pending_raise.active);
    EXPECT_EQ(m_cpu->pending_raise.code, 0xC0000094u);
    EXPECT_EQ(m_cpu->rip, 0x1000u);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 7u);
    EXPECT_EQ(m_cpu->gpr.get32(EDX), 5u);
}

TEST_F(JitExecTest, IdivRm16NativeRaisesDivideErrorWithoutCommitting) {
    struct DelegateGuard {
        DelegateGuard() { setenv("VITREO_JIT_NO_DELEGATE", "1", 1); }
        ~DelegateGuard() { unsetenv("VITREO_JIT_NO_DELEGATE"); }
    } delegate_guard;

    const u8 code[] = {0x66, 0xF7, 0xF9}; // idiv cx
    // DX:AX = -32768 / CX=-1 overflows signed word
    m_cpu->gpr.set32(EAX, 0xAAAA8000u);
    m_cpu->gpr.set32(EDX, 0xBBBBFFFFu);
    m_cpu->gpr.set32(ECX, 0xFFFFFFFFu);
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    ASSERT_TRUE(m_cpu->pending_raise.active);
    EXPECT_TRUE(m_cpu->pending_raise.from_helper);
    EXPECT_EQ(m_cpu->pending_raise.code, 0xC0000094u);
    EXPECT_EQ(m_cpu->rip, 0x1000u);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0xAAAA8000u);
    EXPECT_EQ(m_cpu->gpr.get32(EDX), 0xBBBBFFFFu);
}

TEST_F(JitExecTest, MoffsByteLoadAndStoreAreNativeAndFlagNeutral) {
    struct DelegateGuard {
        DelegateGuard() { setenv("VITREO_JIT_NO_DELEGATE", "1", 1); }
        ~DelegateGuard() { unsetenv("VITREO_JIT_NO_DELEGATE"); }
    } delegate_guard;

    ASSERT_TRUE(m_memory->write_u8(0x2000, 0xA5u));
    m_cpu->gpr.set32(EAX, 0x12345600u);
    m_cpu->flags.rflags = 0xCD7u;
    m_cpu->flag_op = CpuState::FlagOp::None;
    const u8 code[] = {
        0xA0, 0x00, 0x20, 0x00, 0x00, // mov al, byte ptr [0x2000]
        0xA2, 0x01, 0x20, 0x00, 0x00, // mov byte ptr [0x2001], al
    };
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0x123456A5u);
    ASSERT_TRUE(m_memory->read_u8(0x2001).is_ok());
    EXPECT_EQ(m_memory->read_u8(0x2001).value(), 0xA5u);
    EXPECT_EQ(m_cpu->flags.rflags, 0xCD7u);
    EXPECT_EQ(m_cpu->flag_op, CpuState::FlagOp::None);
}

TEST_F(JitExecTest, OrByteFormsAreNativeAndSetLogicalFlags) {
    struct DelegateGuard {
        DelegateGuard() { setenv("VITREO_JIT_NO_DELEGATE", "1", 1); }
        ~DelegateGuard() { unsetenv("VITREO_JIT_NO_DELEGATE"); }
    } delegate_guard;

    ASSERT_TRUE(m_memory->write_u8(0x2000, 0x80u));
    ASSERT_TRUE(m_memory->write_u8(0x2001, 0x80u));
    m_cpu->gpr.set32(ECX, 0x12345601u);
    m_cpu->gpr.set32(EDX, 0xABCDEF02u);
    const u8 code[] = {
        0x80, 0x0D, 0x00, 0x20, 0x00, 0x00, 0x01, // or byte [0x2000],1
        0x08, 0x15, 0x01, 0x20, 0x00, 0x00,       // or byte [0x2001],dl
        0x0A, 0x0D, 0x01, 0x20, 0x00, 0x00,       // or cl,byte [0x2001]
    };
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    ASSERT_TRUE(m_memory->read_u8(0x2000).is_ok());
    ASSERT_TRUE(m_memory->read_u8(0x2001).is_ok());
    EXPECT_EQ(m_memory->read_u8(0x2000).value(), 0x81u);
    EXPECT_EQ(m_memory->read_u8(0x2001).value(), 0x82u);
    EXPECT_EQ(m_cpu->gpr.get32(ECX), 0x12345683u);

    jit_materialize_flags(m_cpu.get());
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.of());
    EXPECT_FALSE(m_cpu->flags.zf());
    EXPECT_TRUE(m_cpu->flags.sf());
    EXPECT_FALSE(m_cpu->flags.pf()); // 0x83 has three set bits
}

TEST_F(JitExecTest, AddSbbAndRegisterXchgByteFormsAreNative) {
    struct DelegateGuard {
        DelegateGuard() { setenv("VITREO_JIT_NO_DELEGATE", "1", 1); }
        ~DelegateGuard() { unsetenv("VITREO_JIT_NO_DELEGATE"); }
    } delegate_guard;

    m_cpu->gpr.set32(EAX, 0x12345601u);
    m_cpu->gpr.set32(ECX, 0xABCDEF02u);
    ASSERT_TRUE(m_memory->write_u8(0x2000, 3));
    const u8 add_xchg_code[] = {
        0x04, 0x04,                               // add al,4 => 5
        0x02, 0x0D, 0x00, 0x20, 0x00, 0x00,       // add cl,[0x2000] => 5
        0x86, 0xC8,                               // xchg al,cl
    };
    EXPECT_EQ(jit_run(add_xchg_code, sizeof(add_xchg_code)),
              JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0x12345605u);
    EXPECT_EQ(m_cpu->gpr.get32(ECX), 0xABCDEF05u);

    m_jit = std::make_unique<JitCompiler>(*m_memory);
    m_cpu->rip = 0x1000;
    m_cpu->gpr.set32(EAX, 0x12345680u);
    m_cpu->flags.rflags = FLAG_CF;
    m_cpu->flag_op = CpuState::FlagOp::None;
    const u8 sbb_code[] = {0x1C, 0x00}; // sbb al,0 => 0x7f
    EXPECT_EQ(jit_run(sbb_code, sizeof(sbb_code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0x1234567Fu);
    jit_materialize_flags(m_cpu.get());
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_TRUE(m_cpu->flags.of());
    EXPECT_TRUE(m_cpu->flags.af());
    EXPECT_FALSE(m_cpu->flags.sf());
}
