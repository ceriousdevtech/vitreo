// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT execution tests — SSE/FP ops between a flag producer and a terminal Jcc

#include "jit_exec_test_fixture.hpp"

TEST_F(JitExecTest, SubThenMinpsThenJnzTerminatesLikeInterpreter) {
    // SUB ecx,1; MINPS xmm0,xmm1; JNZ — the AABB walk shape. MINPS must
    // not kill SUB's ZF, and the loop must stop at ecx=0.
    const u8 code[] = {
        0xB9, 0x04, 0x00, 0x00, 0x00,  // mov ecx, 4
        0x83, 0xE9, 0x01,              // L: sub ecx, 1
        0x0F, 0x5D, 0xC1,              //    minps xmm0, xmm1
        0x75, 0xF8,                    //    jnz L
    };

    auto fill_xmm = [](CpuState& cpu) {
        cpu.sse.xmm[0].f32_[0] = 3.0f;
        cpu.sse.xmm[0].f32_[1] = 1.0f;
        cpu.sse.xmm[0].f32_[2] = 4.0f;
        cpu.sse.xmm[0].f32_[3] = 2.0f;
        cpu.sse.xmm[1].f32_[0] = 2.0f;
        cpu.sse.xmm[1].f32_[1] = 5.0f;
        cpu.sse.xmm[1].f32_[2] = 1.0f;
        cpu.sse.xmm[1].f32_[3] = 8.0f;
    };

    CpuState reference;
    reference.init_protected_mode();
    reference.gpr.set32(ESP, 0x10000);
    fill_xmm(reference);
    reference.rip = 0x1000;
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    Interpreter interpreter(reference, *m_memory);
    (void)interpreter.run(20);

    m_cpu->init_protected_mode();
    m_cpu->gpr.set32(ESP, 0x10000);
    fill_xmm(*m_cpu);
    m_cpu->rip = 0x1000;

    bool done = false;
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
        if (m_cpu->rip == reference.rip) {
            done = true;
            break;
        }
        m_cpu->rip = 0x1000;
    }
    EXPECT_TRUE(done);
    EXPECT_EQ(m_cpu->gpr.get32(ECX), 0u);
    EXPECT_EQ(m_cpu->gpr.get32(ECX), reference.gpr.get32(ECX));
    EXPECT_EQ(m_cpu->rip, reference.rip);
    for (int lane = 0; lane < 4; ++lane) {
        EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[lane],
                        reference.sse.xmm[0].f32_[lane]);
    }
}

TEST_F(JitExecTest, MinpsNanLaneTakesSourceLikeInterpreter) {
    // Intel SDM Vol. 2 MINPS: NaN dest → src. ARM FMIN would keep NaN.
    const u8 code[] = {0x0F, 0x5D, 0xC1};  // minps xmm0, xmm1
    const float qnan = std::numeric_limits<float>::quiet_NaN();

    m_cpu->sse.xmm[0].f32_[0] = qnan;
    m_cpu->sse.xmm[0].f32_[1] = 4.0f;
    m_cpu->sse.xmm[0].f32_[2] = qnan;
    m_cpu->sse.xmm[0].f32_[3] = -1.0f;
    m_cpu->sse.xmm[1].f32_[0] = 2.0f;
    m_cpu->sse.xmm[1].f32_[1] = qnan;
    m_cpu->sse.xmm[1].f32_[2] = 7.0f;
    m_cpu->sse.xmm[1].f32_[3] = -3.0f;

    CpuState reference = *m_cpu;
    reference.rip = 0x1000;
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    Interpreter interpreter(reference, *m_memory);
    (void)interpreter.run(1);

    m_cpu->rip = 0x1000;
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->rip, reference.rip);
    for (int lane = 0; lane < 4; ++lane) {
        const float got = m_cpu->sse.xmm[0].f32_[lane];
        const float exp = reference.sse.xmm[0].f32_[lane];
        if (std::isnan(exp)) {
            EXPECT_TRUE(std::isnan(got)) << "lane " << lane;
        } else {
            EXPECT_FLOAT_EQ(got, exp) << "lane " << lane;
        }
    }
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[0], 2.0f);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[2], 7.0f);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[3], -3.0f);
}

TEST_F(JitExecTest, AddThenCmpThenJlTerminatesLikeInterpreter) {
    // `add eax, 2; cmp eax, 0x18; jl` — the 0x650a60 plane-walk increment.
    const u8 code[] = {
        0x31, 0xC0,              // xor eax, eax
        0x83, 0xC0, 0x02,        // L: add eax, 2
        0x83, 0xF8, 0x18,        //    cmp eax, 0x18
        0x7C, 0xF8,              //    jl L
    };

    CpuState reference;
    reference.init_protected_mode();
    reference.gpr.set32(ESP, 0x10000);
    reference.rip = 0x1000;
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    Interpreter interpreter(reference, *m_memory);
    (void)interpreter.run(40);

    m_cpu->init_protected_mode();
    m_cpu->gpr.set32(ESP, 0x10000);
    m_cpu->rip = 0x1000;
    bool done = false;
    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
        if (m_cpu->rip == reference.rip) {
            done = true;
            break;
        }
        m_cpu->rip = 0x1000;
    }
    EXPECT_TRUE(done);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0x18u);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), reference.gpr.get32(EAX));
    EXPECT_EQ(m_cpu->rip, reference.rip);
}

TEST_F(JitExecTest, CmpThenStoreThenJbTerminatesLikeInterpreter) {
    // `add eax,1; cmp eax, 0x80; mov [esp+0x14], eax; jb` — the
    // 0x672e80 cascade counter. The store must not steal CMP's CF.
    const u8 code[] = {
        0x31, 0xC0,                    // xor eax, eax
        0x83, 0xC0, 0x01,              // L: add eax, 1
        0x3D, 0x80, 0x00, 0x00, 0x00,  //    cmp eax, 0x80
        0x89, 0x44, 0x24, 0x14,        //    mov [esp+0x14], eax
        0x72, 0xF2,                    //    jb L
    };

    CpuState reference;
    reference.init_protected_mode();
    reference.gpr.set32(ESP, 0x2000);
    reference.rip = 0x1000;
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    Interpreter interpreter(reference, *m_memory);
    (void)interpreter.run(800);

    m_cpu->init_protected_mode();
    m_cpu->gpr.set32(ESP, 0x2000);
    m_cpu->rip = 0x1000;
    bool done = false;
    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
        if (m_cpu->rip == reference.rip) {
            done = true;
            break;
        }
        m_cpu->rip = 0x1000;
    }
    EXPECT_TRUE(done);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0x80u);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), reference.gpr.get32(EAX));
    EXPECT_EQ(m_memory->read_u32(0x2014).value_or(0), 0x80u);
    EXPECT_EQ(m_cpu->rip, reference.rip);
}

TEST_F(JitExecTest, CmpThenMovapsStoreThenJeMatchesInterpreter) {
    const u8 code[] = {
        0x39, 0xD8,                    // cmp eax, ebx
        0x0F, 0x29, 0x44, 0x24, 0x10,  // movaps [esp+0x10], xmm0
        0x74, 0x02,                    // je +2
    };
    const std::pair<u32, u32> pairs[] = {
        {5, 3}, {3, 5}, {5, 5},
    };
    for (auto [eax, ebx] : pairs) {
        CpuState reference;
        reference.init_protected_mode();
        reference.gpr.set32(ESP, 0x2000);
        reference.gpr.set32(EAX, eax);
        reference.gpr.set32(EBX, ebx);
        reference.rip = 0x1000;
        ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
        Interpreter interpreter(reference, *m_memory);
        (void)interpreter.run(3);

        m_cpu->init_protected_mode();
        m_cpu->gpr.set32(ESP, 0x2000);
        m_cpu->gpr.set32(EAX, eax);
        m_cpu->gpr.set32(EBX, ebx);
        m_cpu->rip = 0x1000;
        EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
        EXPECT_EQ(m_cpu->rip, reference.rip)
            << "eax=0x" << std::hex << eax << " ebx=0x" << ebx;
    }
}

TEST_F(JitExecTest, CmpThenMovapsStoreThenJaeMatchesInterpreter) {
    // `cmp eax, ebx; movaps [esp+0x10], xmm0; jae` — the sorted-insert
    // capacity check. A 128-bit store's region SUBS must not steal CMP's CF.
    const u8 code[] = {
        0x39, 0xD8,                    // cmp eax, ebx
        0x0F, 0x29, 0x44, 0x24, 0x10,  // movaps [esp+0x10], xmm0
        0x73, 0x02,                    // jae +2
    };
    const std::pair<u32, u32> pairs[] = {
        {5, 3}, {3, 5}, {5, 5}, {0x80000000u, 1}, {1, 0x80000000u},
    };
    for (auto [eax, ebx] : pairs) {
        CpuState reference;
        reference.init_protected_mode();
        reference.gpr.set32(ESP, 0x2000);
        reference.gpr.set32(EAX, eax);
        reference.gpr.set32(EBX, ebx);
        reference.rip = 0x1000;
        ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
        Interpreter interpreter(reference, *m_memory);
        (void)interpreter.run(3);

        m_cpu->init_protected_mode();
        m_cpu->gpr.set32(ESP, 0x2000);
        m_cpu->gpr.set32(EAX, eax);
        m_cpu->gpr.set32(EBX, ebx);
        m_cpu->rip = 0x1000;
        EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
        EXPECT_EQ(m_cpu->rip, reference.rip)
            << "eax=0x" << std::hex << eax << " ebx=0x" << ebx;
    }
}

TEST_F(JitExecTest, SubThenCmpThenJneStopsLikeInterpreter) {
    const u8 code[] = {
        0x83, 0xE9, 0x20,  // L: sub ecx, 0x20
        0x3B, 0xCE,        //    cmp ecx, esi
        0x75, 0xF9,        //    jne L
    };
    const u32 stop = 0x2040;
    const u32 src = stop + 0x60;

    CpuState reference;
    reference.init_protected_mode();
    reference.gpr.set32(ESP, 0x10000);
    reference.gpr.set32(ESI, stop);
    reference.gpr.set32(ECX, src);
    reference.rip = 0x1000;
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    Interpreter interpreter(reference, *m_memory);
    (void)interpreter.run(40);

    m_cpu->init_protected_mode();
    m_cpu->gpr.set32(ESP, 0x10000);
    m_cpu->gpr.set32(ESI, stop);
    m_cpu->gpr.set32(ECX, src);
    m_cpu->rip = 0x1000;
    bool done = false;
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
        if (m_cpu->rip == reference.rip) {
            done = true;
            break;
        }
        m_cpu->rip = 0x1000;
    }
    EXPECT_TRUE(done);
    EXPECT_EQ(m_cpu->gpr.get32(ECX), stop);
    EXPECT_EQ(m_cpu->gpr.get32(ECX), reference.gpr.get32(ECX));
}

TEST_F(JitExecTest, CmpThenMulssThenJneMatchesInterpreter) {
    // `cmp [ecx+8], 0; mulss; jne` — camera setup after a flag-neutral
    // scalar SSE arith whose Inf/NaN gate uses SUBS.
    const u8 code[] = {
        0x39, 0xD8,              // cmp eax, ebx
        0xF3, 0x0F, 0x59, 0xC1,  // mulss xmm0, xmm1
        0x75, 0x02,              // jne +2
    };
    const std::pair<u32, u32> pairs[] = {
        {5, 3}, {3, 5}, {5, 5}, {0x80000000u, 1}, {1, 0x80000000u},
    };
    for (auto [eax, ebx] : pairs) {
        CpuState reference;
        reference.init_protected_mode();
        reference.gpr.set32(ESP, 0x10000);
        reference.gpr.set32(EAX, eax);
        reference.gpr.set32(EBX, ebx);
        reference.sse.xmm[0].f32_[0] = 2.0f;
        reference.sse.xmm[1].f32_[0] = 3.0f;
        reference.rip = 0x1000;
        ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
        Interpreter interpreter(reference, *m_memory);
        (void)interpreter.run(3);

        m_cpu->init_protected_mode();
        m_cpu->gpr.set32(ESP, 0x10000);
        m_cpu->gpr.set32(EAX, eax);
        m_cpu->gpr.set32(EBX, ebx);
        m_cpu->sse.xmm[0].f32_[0] = 2.0f;
        m_cpu->sse.xmm[1].f32_[0] = 3.0f;
        m_cpu->rip = 0x1000;
        EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
        EXPECT_EQ(m_cpu->rip, reference.rip)
            << "eax=0x" << std::hex << eax << " ebx=0x" << ebx;
    }
}

TEST_F(JitExecTest, CmpThenAddpsThenJaeMatchesInterpreter) {
    const u8 code[] = {
        0x39, 0xD8,        // cmp eax, ebx
        0x0F, 0x58, 0xC1,  // addps xmm0, xmm1
        0x73, 0x02,        // jae +2
    };
    const std::pair<u32, u32> pairs[] = {
        {5, 3}, {3, 5}, {5, 5}, {0x80000000u, 1}, {1, 0x80000000u},
    };
    for (auto [eax, ebx] : pairs) {
        CpuState reference;
        reference.init_protected_mode();
        reference.gpr.set32(ESP, 0x10000);
        reference.gpr.set32(EAX, eax);
        reference.gpr.set32(EBX, ebx);
        reference.sse.xmm[0].f32_[0] = 1.0f;
        reference.sse.xmm[1].f32_[0] = 2.0f;
        reference.rip = 0x1000;
        ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
        Interpreter interpreter(reference, *m_memory);
        (void)interpreter.run(3);

        m_cpu->init_protected_mode();
        m_cpu->gpr.set32(ESP, 0x10000);
        m_cpu->gpr.set32(EAX, eax);
        m_cpu->gpr.set32(EBX, ebx);
        m_cpu->sse.xmm[0].f32_[0] = 1.0f;
        m_cpu->sse.xmm[1].f32_[0] = 2.0f;
        m_cpu->rip = 0x1000;
        EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
        EXPECT_EQ(m_cpu->rip, reference.rip)
            << "eax=0x" << std::hex << eax << " ebx=0x" << ebx;
    }
}

TEST_F(JitExecTest, ComissThenSubssThenJaMatchesInterpreter) {
    // `comiss; movaps; subss; ja` — load-tick clamp. SUBSS must not
    // steal COMISS's CF/ZF (Intel SDM Vol. 2 COMISS / SUBSS / JA).
    const u8 code[] = {
        0x0F, 0x2F, 0xC1,        // comiss xmm0, xmm1
        0x0F, 0x28, 0xC0,        // movaps xmm0, xmm0
        0xF3, 0x0F, 0x5C, 0xC1,  // subss xmm0, xmm1
        0x77, 0x02,              // ja +2
    };
    const std::pair<float, float> pairs[] = {
        {2.0f, 1.0f}, {1.0f, 2.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f},
    };
    for (auto [a, b] : pairs) {
        CpuState reference;
        reference.init_protected_mode();
        reference.gpr.set32(ESP, 0x10000);
        reference.sse.xmm[0].f32_[0] = a;
        reference.sse.xmm[1].f32_[0] = b;
        reference.rip = 0x1000;
        ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
        Interpreter interpreter(reference, *m_memory);
        (void)interpreter.run(4);

        m_cpu->init_protected_mode();
        m_cpu->gpr.set32(ESP, 0x10000);
        m_cpu->sse.xmm[0].f32_[0] = a;
        m_cpu->sse.xmm[1].f32_[0] = b;
        m_cpu->rip = 0x1000;
        EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
        EXPECT_EQ(m_cpu->rip, reference.rip)
            << "a=" << a << " b=" << b;
    }
}

// COMISS/UCOMISS/COMISD/UCOMISD followed by every condition-code consumer.
//
// Intel SDM Vol. 2A COMISS/UCOMISS/COMISD/UCOMISD: unordered -> ZF,PF,CF=111;
// greater -> 000; less -> 001; equal -> 100; OF, AF and SF are cleared. The
// JIT may branch on the FCMP's live NZCV instead of the materialized EFLAGS,
// so every one of the 16 condition codes -- including the ones FCMP cannot
// express directly (E/NE need V|Z, S/O/L/G read the cleared SF/OF) -- is run
// through a terminal Jcc, a mid-block Jcc whose taken edge is an in-block
// forward target, SETcc and CMOVcc, in 32- and 64-bit blocks, against the
// interpreter. Operand pairs cover less/greater/equal, +0 vs -0 (equal), and a
// quiet NaN on either side (unordered).
TEST_F(JitExecTest, ScalarFpCompareThenEveryConditionMatchesInterpreter) {
    struct Compare { const char* name; u8 prefix; u8 opcode; bool is_sd; };
    const Compare compares[] = {
        {"comiss", 0x00, 0x2F, false},
        {"ucomiss", 0x00, 0x2E, false},
        {"comisd", 0x66, 0x2F, true},
        {"ucomisd", 0x66, 0x2E, true},
    };
    const double qnan = std::numeric_limits<double>::quiet_NaN();
    const std::pair<double, double> pairs[] = {
        {1.0, 2.0}, {2.0, 1.0}, {1.5, 1.5}, {0.0, -0.0},
        {qnan, 1.0}, {1.0, qnan},
    };
    enum class Consumer { TerminalJcc, MidBlockJcc, Setcc, Cmovcc };
    const Consumer consumers[] = {
        Consumer::TerminalJcc, Consumer::MidBlockJcc, Consumer::Setcc,
        Consumer::Cmovcc,
    };
    constexpr u64 kArithFlags = 0x8D5;  // OF SF ZF AF PF CF

    for (const bool long_mode : {false, true}) {
    for (const Compare& cmp : compares) {
    for (const Consumer consumer : consumers) {
    for (u8 cc = 0; cc < 16; ++cc) {
        std::vector<u8> code;
        if (cmp.prefix != 0) code.push_back(cmp.prefix);
        code.insert(code.end(), {0x0F, cmp.opcode, 0xC1});  // xmm0, xmm1
        usize steps = 2;
        switch (consumer) {
        case Consumer::TerminalJcc:
            code.insert(code.end(), {static_cast<u8>(0x70 + cc), 0x02});
            break;
        case Consumer::MidBlockJcc:
            // jcc +2 over `inc edx`, landing on an in-block NOP.
            code.insert(code.end(), {static_cast<u8>(0x70 + cc), 0x02,
                                     0xFF, 0xC2, 0x90});
            steps = 4;
            break;
        case Consumer::Setcc:
            code.insert(code.end(), {0x0F, static_cast<u8>(0x90 + cc), 0xC0});
            break;
        case Consumer::Cmovcc:
            code.insert(code.end(), {0x0F, static_cast<u8>(0x40 + cc), 0xC1});
            break;
        }
        for (const auto& [a, b] : pairs) {
            auto setup = [&](CpuState& cpu) {
                if (long_mode) {
                    cpu.init_long_mode();
                } else {
                    cpu.init_protected_mode();
                }
                cpu.gpr.set32(ESP, 0x10000);
                cpu.rip = 0x1000;
                cpu.flags.rflags = 0x2 | kArithFlags;
                cpu.gpr.set32(EAX, 0x11111111u);
                cpu.gpr.set32(ECX, 0x22222222u);
                cpu.gpr.set32(EDX, 0x33333333u);
                if (cmp.is_sd) {
                    cpu.sse.xmm[0].f64_[0] = a;
                    cpu.sse.xmm[1].f64_[0] = b;
                } else {
                    cpu.sse.xmm[0].f32_[0] = static_cast<float>(a);
                    cpu.sse.xmm[1].f32_[0] = static_cast<float>(b);
                }
            };
            // Every case rewrites 0x1000; a fresh compiler keeps the
            // self-modifying-code churn limit from routing later cases to the
            // interpreter, which would make them compare it with itself.
            m_jit = std::make_unique<JitCompiler>(*m_memory);
            setup(*m_cpu);
            const JitExecResult result =
                long_mode ? jit_run64(code.data(), code.size())
                          : jit_run(code.data(), code.size());
            EXPECT_EQ(result, JitExecResult::Continue);
            m_cpu->materialize_lazy_flags();
            // Both outcomes of the consumer lie past it, so a JIT that never
            // ran the pair cannot pass by comparing an unexecuted state.
            const usize compare_len = cmp.prefix != 0 ? 4u : 3u;
            const usize consumer_len = consumer == Consumer::TerminalJcc ||
                                               consumer == Consumer::MidBlockJcc
                                           ? 2u : 3u;
            ASSERT_GE(m_cpu->rip, 0x1000u + compare_len + consumer_len);

            // A taken mid-block edge may leave the block at its in-block
            // target, so step the reference to wherever the JIT stopped. A
            // wrong direction still diverges: one side skips `inc edx`.
            CpuState reference;
            setup(reference);
            {
                Interpreter interp(reference, *m_memory);
                for (usize step = 0;
                     step < steps && reference.rip != m_cpu->rip; ++step) {
                    (void)interp.run(1);
                }
            }
            reference.materialize_lazy_flags();

            const auto where = [&] {
                return std::string(cmp.name) + (long_mode ? " x64" : " x86") +
                       " consumer=" +
                       std::to_string(static_cast<int>(consumer)) +
                       " cc=" + std::to_string(cc) + " a=" +
                       std::to_string(a) + " b=" + std::to_string(b);
            };
            EXPECT_EQ(m_cpu->rip, reference.rip) << where();
            EXPECT_EQ(m_cpu->gpr.get64(0), reference.gpr.get64(0)) << where();
            EXPECT_EQ(m_cpu->gpr.get64(2), reference.gpr.get64(2)) << where();
            EXPECT_EQ(m_cpu->flags.rflags & kArithFlags,
                      reference.flags.rflags & kArithFlags) << where();
        }
    }
    }
    }
    }
}

TEST_F(JitExecTest, CmpThenMinpsThenJgeMatchesInterpreter) {
    const u8 code[] = {
        0x39, 0xD8,        // cmp eax, ebx
        0x0F, 0x5D, 0xC1,  // minps xmm0, xmm1
        0x7D, 0x02,        // jge +2
    };
    const std::pair<u32, u32> pairs[] = {
        {5, 3}, {3, 5}, {5, 5}, {0x80000000u, 1}, {1, 0x80000000u},
    };
    for (auto [eax, ebx] : pairs) {
        CpuState reference;
        reference.init_protected_mode();
        reference.gpr.set32(ESP, 0x10000);
        reference.gpr.set32(EAX, eax);
        reference.gpr.set32(EBX, ebx);
        reference.sse.xmm[0].f32_[0] = 1.0f;
        reference.sse.xmm[1].f32_[0] = 0.0f;
        reference.rip = 0x1000;
        ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
        Interpreter interpreter(reference, *m_memory);
        (void)interpreter.run(3);

        m_cpu->init_protected_mode();
        m_cpu->gpr.set32(ESP, 0x10000);
        m_cpu->gpr.set32(EAX, eax);
        m_cpu->gpr.set32(EBX, ebx);
        m_cpu->sse.xmm[0].f32_[0] = 1.0f;
        m_cpu->sse.xmm[1].f32_[0] = 0.0f;
        m_cpu->rip = 0x1000;
        EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
        EXPECT_EQ(m_cpu->rip, reference.rip)
            << "eax=0x" << std::hex << eax << " ebx=0x" << ebx;
    }
}

TEST_F(JitExecTest, CmpThenAddpsQNaNThenJaeMatchesInterpreter) {
    // Inf/NaN ADDPS falls back to jit_sse_op (BLR). Intel SDM Vol. 2
    // ADDPS still does not write EFLAGS, so CMP flags must survive.
    const u8 code[] = {
        0x39, 0xD8,        // cmp eax, ebx
        0x0F, 0x58, 0xC1,  // addps xmm0, xmm1
        0x73, 0x02,        // jae +2
    };
    const std::pair<u32, u32> pairs[] = {
        {5, 3}, {3, 5}, {5, 5}, {0x80000000u, 1}, {1, 0x80000000u},
    };
    for (auto [eax, ebx] : pairs) {
        CpuState reference;
        reference.init_protected_mode();
        reference.gpr.set32(ESP, 0x10000);
        reference.gpr.set32(EAX, eax);
        reference.gpr.set32(EBX, ebx);
        reference.sse.xmm[0].u32_[0] = 0x7FC00001u;
        reference.sse.xmm[1].f32_[0] = 1.0f;
        reference.rip = 0x1000;
        ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
        Interpreter interpreter(reference, *m_memory);
        (void)interpreter.run(3);

        m_cpu->init_protected_mode();
        m_cpu->gpr.set32(ESP, 0x10000);
        m_cpu->gpr.set32(EAX, eax);
        m_cpu->gpr.set32(EBX, ebx);
        m_cpu->sse.xmm[0].u32_[0] = 0x7FC00001u;
        m_cpu->sse.xmm[1].f32_[0] = 1.0f;
        m_cpu->rip = 0x1000;
        EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
        EXPECT_EQ(m_cpu->rip, reference.rip)
            << "eax=0x" << std::hex << eax << " ebx=0x" << ebx;
    }
}
