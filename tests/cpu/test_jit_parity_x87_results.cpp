// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests (32-bit mode): x87 result parity
//
// Fixture and shared helpers: jit_parity_fixture.hpp.

#include "jit_parity_fixture.hpp"

TEST_F(JitFpuParityTest, TransformChainMatchesInterpreterAtEveryPrecision) {
    // The shape SimCity 4's terrain transform uses, ending on the arithmetic
    // op so the block's last instruction is an x87 op: that is the path where
    // the PC==24 helper branch used to leave the block unterminated.
    //   fld dword [0x2000]      D9 05 00 20 00 00
    //   fmul dword [0x2004]     D8 0D 04 20 00 00
    //   fld dword [0x2008]      D9 05 08 20 00 00
    //   fmul dword [0x200C]     D8 0D 0C 20 00 00
    //   faddp st(1), st         DE C1
    const std::vector<u8> code = {
        0xD9, 0x05, 0x00, 0x20, 0x00, 0x00,
        0xD8, 0x0D, 0x04, 0x20, 0x00, 0x00,
        0xD9, 0x05, 0x08, 0x20, 0x00, 0x00,
        0xD8, 0x0D, 0x0C, 0x20, 0x00, 0x00,
        0xDE, 0xC1,
    };
    // Values with significands that do not survive a 24-bit round intact, so a
    // precision mismatch between the engines shows up rather than cancelling.
    const float inputs[4] = {1.4142135f, 3.14159265f, 2.7182818f, 1.6180339f};
    for (u32 i = 0; i < 4; ++i) {
        ASSERT_TRUE(m_memory->write_bytes(
            0x2000 + i * 4, reinterpret_cast<const u8*>(&inputs[i]), 4));
    }

    struct Case { u16 cw; const char* name; };
    const Case cases[] = {
        {0x003Fu, "PC=00 single"},
        {0x023Fu, "PC=10 double"},
        {0x033Fu, "PC=11 extended"},
    };
    for (const auto& c : cases) {
        auto [interp, jit] = st0_both(code, c.cw);
        EXPECT_DOUBLE_EQ(interp, jit)
            << "JIT/interpreter x87 divergence at " << c.name
            << " (cw=0x" << std::hex << c.cw << ")";
    }
}

// Sweep the x87 arithmetic forms rather than spot-checking. Codegen varies per
// form (memory vs ST(i), popping vs not, reversed operand order), and a single
// wrong form is invisible in a game as anything but subtly wrong output. Each
// case runs the same prologue so ST0/ST1 hold known distinct values, then the
// form under test, then compares ST0 against the interpreter.
TEST_F(JitFpuParityTest, ArithmeticFormsMatchInterpreter) {
    struct Form { const char* name; std::vector<u8> tail; };
    const Form forms[] = {
        // ST(i) forms, both directions and both popping variants.
        {"fadd st,st(1)",   {0xD8, 0xC1}},
        {"fadd st(1),st",   {0xDC, 0xC1}},
        {"faddp st(1),st",  {0xDE, 0xC1}},
        {"fsub st,st(1)",   {0xD8, 0xE1}},
        {"fsub st(1),st",   {0xDC, 0xE9}},
        {"fsubp st(1),st",  {0xDE, 0xE9}},
        {"fsubr st,st(1)",  {0xD8, 0xE9}},
        {"fsubrp st(1),st", {0xDE, 0xE1}},
        {"fmul st,st(1)",   {0xD8, 0xC9}},
        {"fmul st(1),st",   {0xDC, 0xC9}},
        {"fmulp st(1),st",  {0xDE, 0xC9}},
        {"fdiv st,st(1)",   {0xD8, 0xF1}},
        {"fdivp st(1),st",  {0xDE, 0xF9}},
        {"fdivr st,st(1)",  {0xD8, 0xF9}},
        // Memory forms, which already passed the index through correctly.
        {"fadd dword[m]",   {0xD8, 0x05, 0x00, 0x20, 0x00, 0x00}},
        {"fmul dword[m]",   {0xD8, 0x0D, 0x00, 0x20, 0x00, 0x00}},
        {"fsub dword[m]",   {0xD8, 0x25, 0x00, 0x20, 0x00, 0x00}},
        {"fdiv dword[m]",   {0xD8, 0x35, 0x00, 0x20, 0x00, 0x00}},
    };
    const float a = 3.14159265f, b = 1.4142135f;
    ASSERT_TRUE(m_memory->write_bytes(0x2000,
                                      reinterpret_cast<const u8*>(&a), 4));
    ASSERT_TRUE(m_memory->write_bytes(0x2004,
                                      reinterpret_cast<const u8*>(&b), 4));

    for (const auto& form : forms) {
        for (u16 cw : {u16{0x003F}, u16{0x023F}, u16{0x033F}}) {
            // fld [0x2004] ; fld [0x2000]  -> ST0=a, ST1=b (distinct, so a
            // wrong register index or operand order cannot cancel out).
            std::vector<u8> code = {
                0xD9, 0x05, 0x04, 0x20, 0x00, 0x00,
                0xD9, 0x05, 0x00, 0x20, 0x00, 0x00,
            };
            code.insert(code.end(), form.tail.begin(), form.tail.end());
            auto [interp, jit] = st0_both(code, cw);
            EXPECT_DOUBLE_EQ(interp, jit)
                << form.name << " diverged at cw=0x" << std::hex << cw;
        }
    }
}

// Stores, integer conversions and stack moves. A wrong *store* is what puts a
// bad value into guest memory where it becomes a pixel, so compare the written
// bytes, not just ST0. Comparing only ST0 would call all of these correct.
TEST_F(JitFpuParityTest, StoreAndConvertFormsMatchInterpreter) {
    struct Form { const char* name; std::vector<u8> tail; u32 addr; u32 len; };
    const Form forms[] = {
        {"fst dword[m]",   {0xD9, 0x15, 0x10, 0x20, 0x00, 0x00}, 0x2010, 4},
        {"fstp dword[m]",  {0xD9, 0x1D, 0x10, 0x20, 0x00, 0x00}, 0x2010, 4},
        {"fst qword[m]",   {0xDD, 0x15, 0x10, 0x20, 0x00, 0x00}, 0x2010, 8},
        {"fstp qword[m]",  {0xDD, 0x1D, 0x10, 0x20, 0x00, 0x00}, 0x2010, 8},
        {"fist dword[m]",  {0xDB, 0x15, 0x10, 0x20, 0x00, 0x00}, 0x2010, 4},
        {"fistp dword[m]", {0xDB, 0x1D, 0x10, 0x20, 0x00, 0x00}, 0x2010, 4},
        {"fistp qword[m]", {0xDF, 0x3D, 0x10, 0x20, 0x00, 0x00}, 0x2010, 8},
        {"fist word[m]",   {0xDF, 0x15, 0x10, 0x20, 0x00, 0x00}, 0x2010, 2},
        {"fxch st(1)",     {0xD9, 0xC9},                         0,      0},
        {"fchs",           {0xD9, 0xE0},                         0,      0},
        {"fabs",           {0xD9, 0xE1},                         0,      0},
        {"fsqrt",          {0xD9, 0xFA},                         0,      0},
        {"frndint",        {0xD9, 0xFC},                         0,      0},
    };
    const float a = 3.14159265f, b = 1.4142135f;
    ASSERT_TRUE(m_memory->write_bytes(0x2000,
                                      reinterpret_cast<const u8*>(&a), 4));
    ASSERT_TRUE(m_memory->write_bytes(0x2004,
                                      reinterpret_cast<const u8*>(&b), 4));

    for (const auto& form : forms) {
        for (u16 cw : {u16{0x003F}, u16{0x023F}, u16{0x033F}}) {
            std::vector<u8> code = {
                0xD9, 0x05, 0x04, 0x20, 0x00, 0x00,   // fld [b] -> ST0=b
                0xD9, 0x05, 0x00, 0x20, 0x00, 0x00,   // fld [a] -> ST0=a, ST1=b
            };
            code.insert(code.end(), form.tail.begin(), form.tail.end());

            // Poison the destination so a store that never happens is caught.
            std::vector<u8> poison(8, 0xCD);
            if (form.len) {
                ASSERT_TRUE(m_memory->write_bytes(form.addr, poison.data(),
                                                  form.len));
            }
            auto [interp_st0, jit_st0] = st0_both_with_mem(
                code, cw, form.addr, form.len);
            EXPECT_DOUBLE_EQ(interp_st0, jit_st0)
                << form.name << " ST0 diverged at cw=0x" << std::hex << cw;
        }
    }
}

// Comparisons, the status word, and stack-pointer discipline. A wrong compare
// sends the guest down a different branch, which shows up as correct geometry
// with wrong shading rather than as a crash -- so compare C0/C2/C3 and the
// whole register stack, not just ST0. TOP is included because an unbalanced
// push/pop makes every later value come from the wrong register.
TEST_F(JitFpuParityTest, CompareStatusAndStackDisciplineMatchInterpreter) {
    struct Form { const char* name; std::vector<u8> tail; };
    const Form forms[] = {
        {"fcom st(1)",    {0xD8, 0xD1}},
        {"fcomp st(1)",   {0xD8, 0xD9}},
        {"fcompp",        {0xDE, 0xD9}},
        {"fucom st(1)",   {0xDD, 0xE1}},
        {"fucomp st(1)",  {0xDD, 0xE9}},
        {"ftst",          {0xD9, 0xE4}},
        {"fxam",          {0xD9, 0xE5}},
        {"fcom dword[m]", {0xD8, 0x15, 0x00, 0x20, 0x00, 0x00}},
        {"fnstsw ax",     {0xDF, 0xE0}},
        {"fld st(1)",     {0xD9, 0xC1}},
        {"fld1",          {0xD9, 0xE8}},
        {"fldz",          {0xD9, 0xEE}},
        {"fldpi",         {0xD9, 0xEB}},
        {"fild dword[m]", {0xDB, 0x05, 0x00, 0x20, 0x00, 0x00}},
        {"fdecstp",       {0xD9, 0xF6}},
        {"fincstp",       {0xD9, 0xF7}},
        {"ffree st(1)",   {0xDD, 0xC1}},
    };
    const float a = 3.14159265f, b = 1.4142135f;
    ASSERT_TRUE(m_memory->write_bytes(0x2000,
                                      reinterpret_cast<const u8*>(&a), 4));
    ASSERT_TRUE(m_memory->write_bytes(0x2004,
                                      reinterpret_cast<const u8*>(&b), 4));

    for (const auto& form : forms) {
        for (u16 cw : {u16{0x003F}, u16{0x023F}, u16{0x033F}}) {
            std::vector<u8> code = {
                0xD9, 0x05, 0x04, 0x20, 0x00, 0x00,
                0xD9, 0x05, 0x00, 0x20, 0x00, 0x00,
            };
            code.insert(code.end(), form.tail.begin(), form.tail.end());

            CpuState icpu; init_cpu(icpu); icpu.fpu.control_word = cw;
            CpuState jcpu; init_cpu(jcpu); jcpu.fpu.control_word = cw;
            ASSERT_TRUE(m_memory->write_bytes(0x1000, code.data(), code.size()));
            BasicBlock block = build_block(code.data(), code.size());
            {
                Interpreter interp(icpu, *m_memory);
                EXPECT_EQ(interp.run(block.instruction_count),
                          ExecResult::Continue);
            }
            {
                JitCompiler jit(*m_memory);
                auto* compiled = jit.compile(block);
                ASSERT_NE(compiled, nullptr) << form.name;
                EXPECT_EQ(test::execute_like_dispatcher(jit, *compiled, jcpu), JitExecResult::Continue);
            }
            EXPECT_EQ(static_cast<u32>(icpu.rip), static_cast<u32>(jcpu.rip))
                << form.name << ": engines stopped at different instructions";
            EXPECT_EQ(icpu.fpu.top(), jcpu.fpu.top())
                << form.name << ": FPU stack TOP diverged at cw=0x"
                << std::hex << cw;
            // C0/C2/C3 carry the comparison outcome the guest branches on.
            constexpr u16 kConditionBits = 0x4700;
            EXPECT_EQ(icpu.fpu.status_word & kConditionBits,
                      jcpu.fpu.status_word & kConditionBits)
                << form.name << ": status condition bits diverged at cw=0x"
                << std::hex << cw;
            for (u8 i = 0; i < 4; ++i) {
                EXPECT_EQ(icpu.fpu.get_st(i).to_double(),
                          jcpu.fpu.get_st(i).to_double())
                    << form.name << ": ST(" << int(i) << ") diverged at cw=0x"
                    << std::hex << cw;
            }
        }
    }
}

// Long mixed sequence. Every single form above passes in isolation, so a
// divergence that only appears in real code has to come from state carried
// *between* instructions: the FPU stack wrapping past TOP=7, scratch registers
// reused across the per-instruction precision guard, or the helper and native
// paths alternating within one block. SimCity 4's terrain block is ~40 x87
// instructions long and mixes loads, multiplies, adds and stores, so exercise
// that shape rather than another isolated opcode.
TEST_F(JitFpuParityTest, LongMixedSequenceMatchesInterpreter) {
    const float vals[8] = {1.4142135f, 3.14159265f, 2.7182818f, 1.6180339f,
                           0.5772156f, 1.2020569f, 2.6854520f, 0.6180339f};
    for (u32 i = 0; i < 8; ++i) {
        ASSERT_TRUE(m_memory->write_bytes(
            0x2000 + i * 4, reinterpret_cast<const u8*>(&vals[i]), 4));
    }

    // Build a transform-shaped chain: repeated (fld m; fmul m; faddp) with
    // periodic stores, deliberately pushing enough to wrap the 8-deep stack.
    std::vector<u8> code;
    auto fld_m  = [&](u32 off) {
        code.insert(code.end(), {0xD9, 0x05});
        for (int i = 0; i < 4; ++i) code.push_back((0x2000 + off) >> (i * 8) & 0xFF);
    };
    auto fmul_m = [&](u32 off) {
        code.insert(code.end(), {0xD8, 0x0D});
        for (int i = 0; i < 4; ++i) code.push_back((0x2000 + off) >> (i * 8) & 0xFF);
    };
    auto fstp_m = [&](u32 off) {
        code.insert(code.end(), {0xD9, 0x1D});
        for (int i = 0; i < 4; ++i) code.push_back((0x2100 + off) >> (i * 8) & 0xFF);
    };
    fld_m(0);
    for (u32 round = 0; round < 6; ++round) {
        fld_m((round * 4) % 32);
        fmul_m(((round + 1) * 4) % 32);
        code.insert(code.end(), {0xDE, 0xC1});          // faddp st(1),st
        if (round % 2 == 1) {
            code.insert(code.end(), {0xD9, 0xC0});      // fld st(0)  (push)
            fstp_m(round * 4);                          // store and pop
        }
    }
    code.insert(code.end(), {0xD9, 0xC9});              // fxch st(1)
    code.insert(code.end(), {0xDE, 0xC1});              // faddp -> ends on FPU op

    for (u16 cw : {u16{0x003F}, u16{0x027F}, u16{0x033F}}) {
        std::vector<u8> scratch(64, 0xCD);
        ASSERT_TRUE(m_memory->write_bytes(0x2100, scratch.data(), 64));
        CpuState icpu; init_cpu(icpu); icpu.fpu.control_word = cw;
        ASSERT_TRUE(m_memory->write_bytes(0x1000, code.data(), code.size()));
        BasicBlock block = build_block(code.data(), code.size());
        {
            Interpreter interp(icpu, *m_memory);
            EXPECT_EQ(interp.run(block.instruction_count), ExecResult::Continue);
        }
        std::vector<u8> interp_mem(64);
        ASSERT_TRUE(m_memory->read_bytes(0x2100, interp_mem.data(), 64));

        ASSERT_TRUE(m_memory->write_bytes(0x2100, scratch.data(), 64));
        CpuState jcpu; init_cpu(jcpu); jcpu.fpu.control_word = cw;
        {
            JitCompiler jit(*m_memory);
            auto* compiled = jit.compile(block);
            ASSERT_NE(compiled, nullptr);
            EXPECT_EQ(test::execute_like_dispatcher(jit, *compiled, jcpu), JitExecResult::Continue);
        }
        std::vector<u8> jit_mem(64);
        ASSERT_TRUE(m_memory->read_bytes(0x2100, jit_mem.data(), 64));

        EXPECT_EQ(static_cast<u32>(icpu.rip), static_cast<u32>(jcpu.rip))
            << "engines stopped at different instructions, cw=0x"
            << std::hex << cw;
        EXPECT_EQ(icpu.fpu.top(), jcpu.fpu.top())
            << "TOP diverged after the sequence, cw=0x" << std::hex << cw;
        EXPECT_EQ(interp_mem, jit_mem)
            << "stored results diverged, cw=0x" << std::hex << cw;
        for (u8 i = 0; i < 8; ++i) {
            // Compare the 80-bit REGISTER, not a double projection of it.
            //
            // Two reasons. The projection is lossy now that arithmetic runs in
            // the architectural format, so two genuinely different registers
            // can compare equal through it. And it cannot express agreement on
            // a NaN at all: once a stack fault puts the indefinite QNaN in a
            // register -- which is the correct result -- `EXPECT_EQ` on
            // doubles fails for two IDENTICAL registers, because NaN != NaN.
            EXPECT_EQ(icpu.fpu.get_st(i).mantissa,
                      jcpu.fpu.get_st(i).mantissa)
                << "ST(" << int(i) << ") significand diverged, cw=0x"
                << std::hex << cw;
            EXPECT_EQ(icpu.fpu.get_st(i).exponent_sign,
                      jcpu.fpu.get_st(i).exponent_sign)
                << "ST(" << int(i) << ") exponent/sign diverged, cw=0x"
                << std::hex << cw;
        }
    }
}
