// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT execution tests — SSE write-through V0 forwarding

#include "jit_exec_test_fixture.hpp"

namespace {

// Like count_word, ignoring the Rt/Rd field (bits 4:0): the vector register
// a CpuState access uses depends on whether the XMM cache holds it.
usize count_word_any_rt(const CompiledBlock& cb, u32 word) {
    usize hits = 0;
    for (const WordRange& range : block_word_ranges(cb)) {
        hits += static_cast<usize>(std::count_if(
            range.begin, range.end,
            [word](u32 w) { return (w & ~0x1Fu) == (word & ~0x1Fu); }));
    }
    return hits;
}

}  // namespace

TEST_F(JitExecTest, Mode32AdjacentSseRrForwardsWriteThroughV0) {
    const u8 code[] = {
        0x66, 0x0F, 0xEF, 0xC1,  // pxor  xmm0,xmm1
        0x66, 0x0F, 0xFE, 0xC2,  // paddd xmm0,xmm2
    };
    for (u8 lane = 0; lane < 4; ++lane) {
        m_cpu->sse.xmm[0].u32_[lane] = 0x10203040u + lane;
        m_cpu->sse.xmm[1].u32_[lane] = 0x01010101u * (lane + 1);
        m_cpu->sse.xmm[2].u32_[lane] = 0x11111111u + lane;
    }

    std::array<u32, 4> expected{};
    for (u8 lane = 0; lane < 4; ++lane) {
        expected[lane] =
            (m_cpu->sse.xmm[0].u32_[lane] ^ m_cpu->sse.xmm[1].u32_[lane])
            + m_cpu->sse.xmm[2].u32_[lane];
    }
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    for (u8 lane = 0; lane < 4; ++lane) {
        EXPECT_EQ(m_cpu->sse.xmm[0].u32_[lane], expected[lane]);
    }

    BasicBlock block = build_block(code, sizeof(code));
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    const u32 xmm0_off = vitreo::cpu::jit::xmm_u64_off(0, 0);
    EXPECT_EQ(count_word(
                  *compiled, vitreo::cpu::jit::arm64::ldr_q_uoff(
                                 0, vitreo::cpu::jit::REG_CPU_STATE,
                                 xmm0_off >> 4)),
              1u)
        << "adjacent PE32 RR SSE chain reloaded its write-through destination";
    EXPECT_EQ(count_word(
                  *compiled, vitreo::cpu::jit::arm64::str_q_uoff(
                                 0, vitreo::cpu::jit::REG_CPU_STATE,
                                 xmm0_off >> 4)),
              2u)
        << "forwarding must keep every architectural XMM store";
}

TEST_F(JitExecTest, X64AdjacentAddpsForwardsWriteThroughV0) {
    const u8 code[] = {
        0x0F, 0x58, 0xC1,  // addps xmm0,xmm1
        0x0F, 0x58, 0xC1,  // addps xmm0,xmm1
    };
    BasicBlock block = build_block64(code, sizeof(code));
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    const u32 xmm0_off = vitreo::cpu::jit::xmm_u64_off(0, 0);
    // Without FPCR.AH each ADDPS keeps a NaN-fallback arm that reloads V0.
    // Under it the XMM register cache holds XMM0 in V16 instead (V0 with
    // VITREO_JIT_XMM_CACHE=0): either way one load, whichever register.
    const u32 arm_reloads = vitreo::math::sse_alternate_nan_handling() ? 0u : 2u;
    EXPECT_EQ(count_word_any_rt(
                  *compiled, vitreo::cpu::jit::arm64::ldr_q_uoff(
                                 0, vitreo::cpu::jit::REG_CPU_STATE,
                                 xmm0_off >> 4)),
              1u + arm_reloads)
        << "two ADDPS instructions need one initial hot load (plus one cold-arm "
           "reload each when gated); more means the adjacent result was lost";
    EXPECT_EQ(count_word_any_rt(
                  *compiled, vitreo::cpu::jit::arm64::str_q_uoff(
                                 0, vitreo::cpu::jit::REG_CPU_STATE,
                                 xmm0_off >> 4)),
              2u)
        << "forwarding must keep every architectural XMM store";
}

TEST_F(JitExecTest, X64AdjacentAddpsSpecialValuesMatchInterpreter) {
    const u8 code[] = {
        0x0F, 0x58, 0xC1,  // addps xmm0,xmm1
        0x0F, 0x58, 0xC1,  // addps xmm0,xmm1
    };
    struct Case {
        std::array<u32, 4> dst;
        std::array<u32, 4> src;
        u32 mxcsr;
    };
    constexpr Case cases[] = {
        {{{0x3F800000u, 0x40000000u, 0x40400000u, 0x40800000u}},
         {{0x3F000000u, 0xBF000000u, 0x3E800000u, 0xBE800000u}}, 0x1F80u},
        // The first helper fallback must republish V0 for the second ADDPS.
        {{{0x7FC00001u, 0x7F800000u, 0x00000001u, 0x80000000u}},
         {{0x7F800001u, 0xFF800000u, 0x00000001u, 0x00000000u}}, 0x1F80u},
        // Round toward -infinity; ADDPS obeys MXCSR.RC and does not write
        // EFLAGS (Intel SDM Vol. 2, ADDPS).
        {{{0x3F800000u, 0xBF800000u, 0x7F7FFFFFu, 0xFF7FFFFFu}},
         {{0x33800000u, 0x33800000u, 0x7F7FFFFFu, 0x7F7FFFFFu}}, 0x3F80u},
    };

    for (const Case& c : cases) {
        CpuState reference;
        reference.init_long_mode();
        reference.gpr.rsp = 0x10000;
        reference.rip = 0x1000;
        reference.sse.mxcsr = c.mxcsr;
        for (u8 lane = 0; lane < 4; ++lane) {
            reference.sse.xmm[0].u32_[lane] = c.dst[lane];
            reference.sse.xmm[1].u32_[lane] = c.src[lane];
        }
        ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
        Interpreter interpreter(reference, *m_memory);
        (void)interpreter.run(2);

        m_cpu->init_long_mode();
        m_cpu->gpr.rsp = 0x10000;
        m_cpu->rip = 0x1000;
        m_cpu->sse.mxcsr = c.mxcsr;
        for (u8 lane = 0; lane < 4; ++lane) {
            m_cpu->sse.xmm[0].u32_[lane] = c.dst[lane];
            m_cpu->sse.xmm[1].u32_[lane] = c.src[lane];
        }
        EXPECT_EQ(jit_run64(code, sizeof(code)), JitExecResult::Continue);
        for (u8 lane = 0; lane < 4; ++lane) {
            EXPECT_EQ(m_cpu->sse.xmm[0].u32_[lane],
                      reference.sse.xmm[0].u32_[lane]);
        }
        EXPECT_EQ(m_cpu->sse.mxcsr, reference.sse.mxcsr);
    }
}

TEST_F(JitExecTest, Mode32LegacySseSelfXorSkipsBothOperandLoads) {
    const u8 code[] = {
        0x66, 0x0F, 0xEF, 0xC0,  // pxor  xmm0,xmm0
        0x0F, 0x57, 0xC0,        // xorps xmm0,xmm0
        0x66, 0x0F, 0x57, 0xC0,  // xorpd xmm0,xmm0
    };
    m_cpu->sse.xmm[0].u64_[0] = 0x0123456789ABCDEFull;
    m_cpu->sse.xmm[0].u64_[1] = 0xFEDCBA9876543210ull;

    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[0], 0u);
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[1], 0u);

    BasicBlock block = build_block(code, sizeof(code));
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    const u32 xmm0_off = vitreo::cpu::jit::xmm_u64_off(0, 0);
    EXPECT_EQ(count_word(
                  *compiled, vitreo::cpu::jit::arm64::ldr_q_uoff(
                                 0, vitreo::cpu::jit::REG_CPU_STATE,
                                 xmm0_off >> 4)),
              0u);
    EXPECT_EQ(count_word(
                  *compiled, vitreo::cpu::jit::arm64::ldr_q_uoff(
                                 1, vitreo::cpu::jit::REG_CPU_STATE,
                                 xmm0_off >> 4)),
              0u);
    EXPECT_EQ(count_word(*compiled,
                         vitreo::cpu::jit::arm64::eor_v16b(0, 0, 0)),
              3u);
    EXPECT_EQ(count_word(
                  *compiled, vitreo::cpu::jit::arm64::str_q_uoff(
                                 0, vitreo::cpu::jit::REG_CPU_STATE,
                                 xmm0_off >> 4)),
              3u)
        << "zero idiom must remain architecturally write-through";

    const u8 nonself[] = {0x66, 0x0F, 0xEF, 0xC1};  // pxor xmm0,xmm1
    block = build_block(nonself, sizeof(nonself));
    compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    const u32 xmm1_off = vitreo::cpu::jit::xmm_u64_off(1, 0);
    EXPECT_EQ(count_word(
                  *compiled, vitreo::cpu::jit::arm64::ldr_q_uoff(
                                 0, vitreo::cpu::jit::REG_CPU_STATE,
                                 xmm0_off >> 4)),
              1u);
    EXPECT_EQ(count_word(
                  *compiled, vitreo::cpu::jit::arm64::ldr_q_uoff(
                                 1, vitreo::cpu::jit::REG_CPU_STATE,
                                 xmm1_off >> 4)),
              1u);
}

TEST_F(JitExecTest, Mode32SseMoveSourceAliasFeedsFiniteFp) {
    const u8 code[] = {
        0x0F, 0x28, 0xD0,  // movaps xmm2,xmm0
        0x0F, 0x59, 0xD0,  // mulps  xmm2,xmm0
    };
    auto run_case = [&](u32 lane0) {
        m_cpu->rip = 0x1000;
        for (u8 lane = 0; lane < 4; ++lane) {
            m_cpu->sse.xmm[0].u32_[lane] = 0x3F800000u + lane;
            m_cpu->sse.xmm[2].u32_[lane] = 0xA5A50000u + lane;
        }
        m_cpu->sse.xmm[0].u32_[0] = lane0;
        CpuState reference = *m_cpu;
        ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
        Interpreter interpreter(reference, *m_memory);
        (void)interpreter.run(2);
        EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
        EXPECT_EQ(m_cpu->sse.xmm[2].u64_[0], reference.sse.xmm[2].u64_[0]);
        EXPECT_EQ(m_cpu->sse.xmm[2].u64_[1], reference.sse.xmm[2].u64_[1]);
    };
    run_case(0x40000000u);  // finite
    run_case(0x7FC12345u);  // QNaN helper arm

    BasicBlock block = build_block(code, sizeof(code));
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    const u32 xmm0_off = vitreo::cpu::jit::xmm_u64_off(0, 0);
    EXPECT_EQ(count_word(
                  *compiled, vitreo::cpu::jit::arm64::ldr_q_uoff(
                                 1, vitreo::cpu::jit::REG_CPU_STATE,
                                 xmm0_off >> 4)),
              0u);
    EXPECT_TRUE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::fmul_v(
                       0, 0, 0, vitreo::cpu::jit::arm64::FS_S)));

    const u8 div_code[] = {
        0x0F, 0x28, 0xD0,  // movaps xmm2,xmm0
        0x0F, 0x5E, 0xD0,  // divps  xmm2,xmm0
    };
    for (u32 lane0 : {0x40000000u, 0x00000000u, 0x7FC12345u}) {
        m_cpu->rip = 0x1000;
        for (u8 lane = 0; lane < 4; ++lane) {
            m_cpu->sse.xmm[0].u32_[lane] = 0x40000000u;
        }
        m_cpu->sse.xmm[0].u32_[0] = lane0;
        CpuState reference = *m_cpu;
        ASSERT_TRUE(m_memory->write_bytes(0x1000, div_code,
                                          sizeof(div_code)));
        Interpreter interpreter(reference, *m_memory);
        (void)interpreter.run(2);
        EXPECT_EQ(jit_run(div_code, sizeof(div_code)),
                  JitExecResult::Continue);
        EXPECT_EQ(m_cpu->sse.xmm[2].u64_[0], reference.sse.xmm[2].u64_[0]);
        EXPECT_EQ(m_cpu->sse.xmm[2].u64_[1], reference.sse.xmm[2].u64_[1]);
    }

    const u8 expired[] = {
        0x0F, 0x28, 0xD0,  // movaps xmm2,xmm0
        0x90,              // nop expires one-instruction alias
        0x0F, 0x59, 0xD0,  // mulps xmm2,xmm0
    };
    block = build_block(expired, sizeof(expired));
    compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    EXPECT_EQ(count_word(
                  *compiled, vitreo::cpu::jit::arm64::ldr_q_uoff(
                                 1, vitreo::cpu::jit::REG_CPU_STATE,
                                 xmm0_off >> 4)),
              1u)
        << "source value equivalence survived beyond one x86 instruction";
}

TEST_F(JitExecTest, Mode32RsqrtpsConsumesAndPublishesWriteThroughV0) {
    const u8 standalone[] = {0x0F, 0x52, 0xC1};  // rsqrtps xmm0,xmm1
    BasicBlock block = build_block(standalone, sizeof(standalone));
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    const u32 xmm0_off = vitreo::cpu::jit::xmm_u64_off(0, 0);
    const u32 xmm1_off = vitreo::cpu::jit::xmm_u64_off(1, 0);
    EXPECT_EQ(count_word(
                  *compiled, vitreo::cpu::jit::arm64::ldr_q_uoff(
                                 0, vitreo::cpu::jit::REG_CPU_STATE,
                                 xmm1_off >> 4)),
              1u);
    EXPECT_EQ(count_word(
                  *compiled, vitreo::cpu::jit::arm64::ldr_q_uoff(
                                 0, vitreo::cpu::jit::REG_CPU_STATE,
                                 xmm0_off >> 4)),
              0u)
        << "unary RSQRTPS must not load its dead destination";
    EXPECT_EQ(count_word(
                  *compiled, vitreo::cpu::jit::arm64::ldr_q_uoff(
                                 1, vitreo::cpu::jit::REG_CPU_STATE,
                                 xmm1_off >> 4)),
              0u);
    EXPECT_TRUE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::fsqrt_v(
                       0, 0, vitreo::cpu::jit::arm64::FS_S)));

    const u8 chain[] = {
        0x0F, 0x28, 0xC1,  // movaps  xmm0,xmm1
        0x0F, 0x52, 0xC0,  // rsqrtps xmm0,xmm0
        0x0F, 0x28, 0xD0,  // movaps  xmm2,xmm0
    };
    m_cpu->rip = 0x1000;
    const u32 lanes[] = {
        0x40800000u, 0x41100000u, 0x41800000u, 0x41C80000u,
    };
    for (u8 lane = 0; lane < 4; ++lane) {
        m_cpu->sse.xmm[0].u32_[lane] = 0xA5A50000u + lane;
        m_cpu->sse.xmm[1].u32_[lane] = lanes[lane];
        m_cpu->sse.xmm[2].u32_[lane] = 0x5A5A0000u + lane;
    }
    CpuState reference = *m_cpu;
    ASSERT_TRUE(m_memory->write_bytes(0x1000, chain, sizeof(chain)));
    Interpreter interpreter(reference, *m_memory);
    ASSERT_EQ(interpreter.run(3), ExecResult::Continue);
    EXPECT_EQ(jit_run(chain, sizeof(chain)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[0], reference.sse.xmm[0].u64_[0]);
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[1], reference.sse.xmm[0].u64_[1]);
    EXPECT_EQ(m_cpu->sse.xmm[2].u64_[0], reference.sse.xmm[2].u64_[0]);
    EXPECT_EQ(m_cpu->sse.xmm[2].u64_[1], reference.sse.xmm[2].u64_[1]);

    block = build_block(chain, sizeof(chain));
    block.start_addr = 0x3000;
    compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    EXPECT_EQ(count_word(
                  *compiled, vitreo::cpu::jit::arm64::ldr_q_uoff(
                                 0, vitreo::cpu::jit::REG_CPU_STATE,
                                 xmm0_off >> 4)),
              0u)
        << "adjacent copy, RSQRTPS, and result copy must keep V0 live";
    EXPECT_EQ(count_word(
                  *compiled, vitreo::cpu::jit::arm64::ldr_q_uoff(
                                 1, vitreo::cpu::jit::REG_CPU_STATE,
                                 xmm0_off >> 4)),
              0u);
    EXPECT_TRUE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::fdiv_v(
                       0, 2, 0, vitreo::cpu::jit::arm64::FS_S)));

    const u8 rcp[] = {0x0F, 0x53, 0xC1};  // rcpps xmm0,xmm1
    block = build_block(rcp, sizeof(rcp));
    block.start_addr = 0x4000;
    compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    EXPECT_EQ(count_word(
                  *compiled, vitreo::cpu::jit::arm64::ldr_q_uoff(
                                 0, vitreo::cpu::jit::REG_CPU_STATE,
                                 xmm0_off >> 4)),
              0u);
    EXPECT_TRUE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::fdiv_v(
                       0, 2, 0, vitreo::cpu::jit::arm64::FS_S)));
}

TEST_F(JitExecTest, Mode32SseForwardingStopsAtNonWhitelistedInstruction) {
    const u8 code[] = {
        0x66, 0x0F, 0xEF, 0xC1,  // pxor  xmm0,xmm1
        0x90,                    // nop
        0x66, 0x0F, 0xFE, 0xC2,  // paddd xmm0,xmm2
    };
    BasicBlock block = build_block(code, sizeof(code));
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    const u32 xmm0_off = vitreo::cpu::jit::xmm_u64_off(0, 0);
    EXPECT_EQ(count_word(
                  *compiled, vitreo::cpu::jit::arm64::ldr_q_uoff(
                                 0, vitreo::cpu::jit::REG_CPU_STATE,
                                 xmm0_off >> 4)),
              2u)
        << "forwarding escaped its adjacent whitelisted-instruction boundary";
}

TEST_F(JitExecTest, Mode32SseRegOnlyFamilyJoinsV0ForwardingChain) {
    const u8 code[] = {
        0x66, 0x0F, 0xEF, 0xC1,  // pxor  xmm0,xmm1
        0x66, 0x0F, 0xED, 0xC2,  // paddsw xmm0,xmm2
        0x66, 0x0F, 0xFE, 0xC3,  // paddd xmm0,xmm3
    };
    for (u8 lane = 0; lane < 16; ++lane) {
        m_cpu->sse.xmm[0].u8_[lane] = static_cast<u8>(0x10u + lane);
        m_cpu->sse.xmm[1].u8_[lane] = static_cast<u8>(lane);
        m_cpu->sse.xmm[2].u8_[lane] = static_cast<u8>(0x01u + lane);
        m_cpu->sse.xmm[3].u8_[lane] = static_cast<u8>(0x02u + lane);
    }

    CpuState reference = *m_cpu;
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    Interpreter interpreter(reference, *m_memory);
    (void)interpreter.run(3);

    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[0], reference.sse.xmm[0].u64_[0]);
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[1], reference.sse.xmm[0].u64_[1]);

    BasicBlock block = build_block(code, sizeof(code));
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    const u32 xmm0_off = vitreo::cpu::jit::xmm_u64_off(0, 0);
    EXPECT_EQ(count_word(
                  *compiled, vitreo::cpu::jit::arm64::ldr_q_uoff(
                                 0, vitreo::cpu::jit::REG_CPU_STATE,
                                 xmm0_off >> 4)),
              1u)
        << "mixed standard/reg-only PE32 SSE chain reloaded V0";
    EXPECT_EQ(count_word(
                  *compiled, vitreo::cpu::jit::arm64::str_q_uoff(
                                 0, vitreo::cpu::jit::REG_CPU_STATE,
                                 xmm0_off >> 4)),
              3u)
        << "mixed forwarding chain must remain architecturally write-through";
}

TEST_F(JitExecTest, Mode32SseForwardingDoesNotCrossCompiledBlocks) {
    const u8 producer_code[] = {0x66, 0x0F, 0xEF, 0xC1};  // pxor xmm0,xmm1
    BasicBlock producer = build_block(producer_code, sizeof(producer_code));
    ASSERT_NE(m_jit->compile(producer), nullptr);

    const u8 consumer_code[] = {0x66, 0x0F, 0xFE, 0xC2};  // paddd xmm0,xmm2
    BasicBlock consumer = build_block(consumer_code, sizeof(consumer_code));
    consumer.start_addr = 0x2000;
    CompiledBlock* compiled = m_jit->compile(consumer);
    ASSERT_NE(compiled, nullptr);
    const u32 xmm0_off = vitreo::cpu::jit::xmm_u64_off(0, 0);
    EXPECT_EQ(count_word(
                  *compiled, vitreo::cpu::jit::arm64::ldr_q_uoff(
                                 0, vitreo::cpu::jit::REG_CPU_STATE,
                                 xmm0_off >> 4)),
              1u)
        << "compile-time XMM forwarding state leaked across block entries";
}

TEST_F(JitExecTest, Mode32SseRegisterMoveBridgesV0ForwardingChain) {
    const u8 code[] = {
        0x66, 0x0F, 0xEF, 0xC1,  // pxor  xmm0,xmm1
        0x0F, 0x28, 0xD0,        // movaps xmm2,xmm0
        0x66, 0x0F, 0xFE, 0xD3,  // paddd xmm2,xmm3
    };
    for (u8 lane = 0; lane < 4; ++lane) {
        m_cpu->sse.xmm[0].u32_[lane] = 0x10203040u + lane;
        m_cpu->sse.xmm[1].u32_[lane] = 0x01010101u + lane;
        m_cpu->sse.xmm[3].u32_[lane] = 0x11111111u + lane;
    }

    CpuState reference = *m_cpu;
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    Interpreter interpreter(reference, *m_memory);
    (void)interpreter.run(3);

    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[2].u64_[0], reference.sse.xmm[2].u64_[0]);
    EXPECT_EQ(m_cpu->sse.xmm[2].u64_[1], reference.sse.xmm[2].u64_[1]);

    BasicBlock block = build_block(code, sizeof(code));
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    const u32 xmm0_off = vitreo::cpu::jit::xmm_u64_off(0, 0);
    const u32 xmm2_off = vitreo::cpu::jit::xmm_u64_off(2, 0);
    EXPECT_EQ(count_word(
                  *compiled, vitreo::cpu::jit::arm64::ldr_q_uoff(
                                 0, vitreo::cpu::jit::REG_CPU_STATE,
                                 xmm0_off >> 4)),
              1u);
    EXPECT_EQ(count_word(
                  *compiled, vitreo::cpu::jit::arm64::ldr_q_uoff(
                                 0, vitreo::cpu::jit::REG_CPU_STATE,
                                 xmm2_off >> 4)),
              0u)
        << "MOVAPS failed to bridge the PE32 V0 forwarding chain";
    EXPECT_EQ(count_word(
                  *compiled, vitreo::cpu::jit::arm64::str_q_uoff(
                                 0, vitreo::cpu::jit::REG_CPU_STATE,
                                 xmm0_off >> 4)),
              1u);
    EXPECT_EQ(count_word(
                  *compiled, vitreo::cpu::jit::arm64::str_q_uoff(
                                 0, vitreo::cpu::jit::REG_CPU_STATE,
                                 xmm2_off >> 4)),
              2u)
        << "MOVAPS forwarding must retain both XMM2 write-through stores";
}

TEST_F(JitExecTest, Mode32SseForwardingExcludesOriginalVexBridge) {
    const u8 code[] = {
        0x66, 0x0F, 0xEF, 0xC5,  // pxor   xmm0,xmm5
        0xC5, 0xF1, 0xFE, 0xC2,  // vpaddd xmm0,xmm1,xmm2
        0x66, 0x0F, 0xFE, 0xC3,  // paddd  xmm0,xmm3
    };
    for (u8 lane = 0; lane < 4; ++lane) {
        for (u8 reg = 0; reg < 6; ++reg) {
            m_cpu->sse.xmm[reg].u32_[lane] =
                0x01020304u * (reg + 1) + lane;
        }
    }

    CpuState reference = *m_cpu;
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    Interpreter interpreter(reference, *m_memory);
    (void)interpreter.run(3);

    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[0], reference.sse.xmm[0].u64_[0]);
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[1], reference.sse.xmm[0].u64_[1]);

    BasicBlock block = build_block(code, sizeof(code));
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    const u32 xmm0_off = vitreo::cpu::jit::xmm_u64_off(0, 0);
    EXPECT_EQ(count_word(
                  *compiled, vitreo::cpu::jit::arm64::ldr_q_uoff(
                                 0, vitreo::cpu::jit::REG_CPU_STATE,
                                 xmm0_off >> 4)),
              3u)
        << "legacy forwarding crossed an original VEX.128 bridge";
}

TEST_F(JitExecTest, Mode32FiniteFpJoinsV0ForwardingOnBothRuntimeArms) {
    const u8 code[] = {
        0x66, 0x0F, 0xEF, 0xC0,  // pxor  xmm0,xmm0
        0x0F, 0x59, 0xC1,        // mulps xmm0,xmm1
        0x66, 0x0F, 0xFE, 0xC2,  // paddd xmm0,xmm2
    };

    auto run_case = [&](u32 lane0) {
        m_cpu->rip = 0x1000;
        for (u8 lane = 0; lane < 4; ++lane) {
            m_cpu->sse.xmm[0].u32_[lane] = 0xA5A50000u + lane;
            m_cpu->sse.xmm[1].u32_[lane] = 0x3F800000u;  // 1.0f
            m_cpu->sse.xmm[2].u32_[lane] = 0x01020304u + lane;
        }
        m_cpu->sse.xmm[1].u32_[0] = lane0;

        CpuState reference = *m_cpu;
        ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
        Interpreter interpreter(reference, *m_memory);
        (void)interpreter.run(3);

        EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
        EXPECT_EQ(m_cpu->sse.xmm[0].u64_[0], reference.sse.xmm[0].u64_[0]);
        EXPECT_EQ(m_cpu->sse.xmm[0].u64_[1], reference.sse.xmm[0].u64_[1]);
    };

    run_case(0x40000000u);  // finite native arm: 2.0f
    run_case(0x7FC12345u);  // QNaN forces the whole-instruction helper arm

    BasicBlock block = build_block(code, sizeof(code));
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    const u32 xmm0_off = vitreo::cpu::jit::xmm_u64_off(0, 0);
    // The one reload is the cold helper arm's; with FPCR.AH there is no arm.
    EXPECT_EQ(count_word(
                  *compiled, vitreo::cpu::jit::arm64::ldr_q_uoff(
                                 0, vitreo::cpu::jit::REG_CPU_STATE,
                                 xmm0_off >> 4)),
              vitreo::math::sse_alternate_nan_handling() ? 0u : 1u)
        << "self-XOR must skip both inputs; packed-FP forwarding must omit "
           "the hot destination load, reload after the cold helper (when one "
           "exists), and feed the following integer consumer";
    EXPECT_EQ(count_word(
                  *compiled, vitreo::cpu::jit::arm64::str_q_uoff(
                                 0, vitreo::cpu::jit::REG_CPU_STATE,
                                 xmm0_off >> 4)),
              3u)
        << "packed-FP forwarding must remain architecturally write-through";
}
