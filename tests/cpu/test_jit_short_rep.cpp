// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT execution tests — REP MOVS/STOS lowering and the short-REP knob

#include "jit_exec_test_fixture.hpp"

TEST_F(JitExecTest, RepMovsdProtectedModeUses32BitIndexAndCount) {
    // Intel SDM Vol. 2 MOVS/REP: AddressSize=32 uses ESI, EDI, ECX.
    const u32 words[4] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
    ASSERT_TRUE(m_memory->write_bytes(
        0x2000, reinterpret_cast<const u8*>(words), sizeof(words)));

    m_cpu->gpr.set64(ESI, 0x2000ull | (0xABull << 40));
    m_cpu->gpr.set64(EDI, 0x3000ull | (0xCDull << 40));
    m_cpu->gpr.set64(ECX, 4ull | (0xEFull << 40));

    const u8 code[] = {0xF3, 0xA5};  // REP MOVSD
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);

    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(m_memory->read_u32(0x3000 + static_cast<u32>(i) * 4).value_or(0),
                  words[i]);
    }
    EXPECT_EQ(m_cpu->gpr.get32(ESI), 0x2010u);
    EXPECT_EQ(m_cpu->gpr.get32(EDI), 0x3010u);
    EXPECT_EQ(m_cpu->gpr.get32(ECX), 0u);
}

TEST_F(JitExecTest, ShortRepMovsbMatchesInterpreter) {
    const u8 src[] = {0x11, 0x22, 0x33, 0x44, 0x55};
    ASSERT_TRUE(m_memory->write_bytes(0x2000, src, sizeof(src)));
    m_cpu->gpr.set32(ESI, 0x2000);
    m_cpu->gpr.set32(EDI, 0x3000);
    m_cpu->gpr.set32(ECX, 5);
    const u8 code[] = {0xF3, 0xA4};  // REP MOVSB

    CpuState icpu = *m_cpu;
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    std::array<u8, 5> before{};
    ASSERT_TRUE(m_memory->read_bytes(0x3000, before.data(), before.size()));
    {
        Interpreter interp(icpu, *m_memory);
        EXPECT_EQ(interp.run(1), ExecResult::Continue);
    }
    std::array<u8, 5> interp_dst{};
    ASSERT_TRUE(m_memory->read_bytes(0x3000, interp_dst.data(), interp_dst.size()));
    ASSERT_TRUE(m_memory->write_bytes(0x3000, before.data(), before.size()));

    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    std::array<u8, 5> jit_dst{};
    ASSERT_TRUE(m_memory->read_bytes(0x3000, jit_dst.data(), jit_dst.size()));
    EXPECT_EQ(jit_dst, interp_dst);
    EXPECT_EQ(m_cpu->gpr.get32(ESI), icpu.gpr.get32(ESI));
    EXPECT_EQ(m_cpu->gpr.get32(EDI), icpu.gpr.get32(EDI));
    EXPECT_EQ(m_cpu->gpr.get32(ECX), icpu.gpr.get32(ECX));
}

TEST_F(JitExecTest, ShortRepMovsbDfBackwardMatchesInterpreter) {
    const u8 src[] = {0xAA, 0xBB, 0xCC, 0xDD};
    ASSERT_TRUE(m_memory->write_bytes(0x2000, src, sizeof(src)));
    m_cpu->gpr.set32(ESI, 0x2003);
    m_cpu->gpr.set32(EDI, 0x3003);
    m_cpu->gpr.set32(ECX, 4);
    m_cpu->flags.set_df(true);
    const u8 code[] = {0xF3, 0xA4};

    CpuState icpu = *m_cpu;
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    std::array<u8, 4> before{};
    ASSERT_TRUE(m_memory->read_bytes(0x3000, before.data(), before.size()));
    {
        Interpreter interp(icpu, *m_memory);
        EXPECT_EQ(interp.run(1), ExecResult::Continue);
    }
    std::array<u8, 4> interp_dst{};
    ASSERT_TRUE(m_memory->read_bytes(0x3000, interp_dst.data(), interp_dst.size()));
    ASSERT_TRUE(m_memory->write_bytes(0x3000, before.data(), before.size()));

    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    std::array<u8, 4> jit_dst{};
    ASSERT_TRUE(m_memory->read_bytes(0x3000, jit_dst.data(), jit_dst.size()));
    EXPECT_EQ(jit_dst, interp_dst);
    EXPECT_EQ(m_cpu->gpr.get32(ESI), icpu.gpr.get32(ESI));
    EXPECT_EQ(m_cpu->gpr.get32(EDI), icpu.gpr.get32(EDI));
    EXPECT_EQ(m_cpu->gpr.get32(ECX), 0u);
    EXPECT_TRUE(m_cpu->flags.df());
}

TEST_F(JitExecTest, ShortRepMovsbOverlapMatchesInterpreter) {
    // dest = src+1: architectural smear, not memmove.
    u8 src[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    ASSERT_TRUE(m_memory->write_bytes(0x2000, src, sizeof(src)));
    m_cpu->gpr.set32(ESI, 0x2000);
    m_cpu->gpr.set32(EDI, 0x2001);
    m_cpu->gpr.set32(ECX, 6);
    const u8 code[] = {0xF3, 0xA4};

    CpuState icpu = *m_cpu;
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    std::array<u8, 8> before{};
    ASSERT_TRUE(m_memory->read_bytes(0x2000, before.data(), before.size()));
    {
        Interpreter interp(icpu, *m_memory);
        EXPECT_EQ(interp.run(1), ExecResult::Continue);
    }
    std::array<u8, 8> interp_dst{};
    ASSERT_TRUE(m_memory->read_bytes(0x2000, interp_dst.data(), interp_dst.size()));
    ASSERT_TRUE(m_memory->write_bytes(0x2000, before.data(), before.size()));

    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    std::array<u8, 8> jit_dst{};
    ASSERT_TRUE(m_memory->read_bytes(0x2000, jit_dst.data(), jit_dst.size()));
    EXPECT_EQ(jit_dst, interp_dst);
    EXPECT_EQ(m_cpu->gpr.get32(ESI), icpu.gpr.get32(ESI));
    EXPECT_EQ(m_cpu->gpr.get32(EDI), icpu.gpr.get32(EDI));
    EXPECT_EQ(m_cpu->gpr.get32(ECX), icpu.gpr.get32(ECX));
}

TEST_F(JitExecTest, LongRepMovsbUsesHelperAndMatchesInterpreter) {
    std::array<u8, 80> src{};
    for (u32 i = 0; i < src.size(); ++i) src[i] = static_cast<u8>(i + 1);
    ASSERT_TRUE(m_memory->write_bytes(0x2000, src.data(), src.size()));
    m_cpu->gpr.set32(ESI, 0x2000);
    m_cpu->gpr.set32(EDI, 0x3000);
    m_cpu->gpr.set32(ECX, 80);  // above the short-path threshold
    const u8 code[] = {0xF3, 0xA4};

    CpuState icpu = *m_cpu;
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    std::array<u8, 80> before{};
    ASSERT_TRUE(m_memory->read_bytes(0x3000, before.data(), before.size()));
    {
        Interpreter interp(icpu, *m_memory);
        EXPECT_EQ(interp.run(1), ExecResult::Continue);
    }
    std::array<u8, 80> interp_dst{};
    ASSERT_TRUE(m_memory->read_bytes(0x3000, interp_dst.data(), interp_dst.size()));
    ASSERT_TRUE(m_memory->write_bytes(0x3000, before.data(), before.size()));

    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    std::array<u8, 80> jit_dst{};
    ASSERT_TRUE(m_memory->read_bytes(0x3000, jit_dst.data(), jit_dst.size()));
    EXPECT_EQ(jit_dst, interp_dst);
    EXPECT_EQ(m_cpu->gpr.get32(ESI), icpu.gpr.get32(ESI));
    EXPECT_EQ(m_cpu->gpr.get32(EDI), icpu.gpr.get32(EDI));
    EXPECT_EQ(m_cpu->gpr.get32(ECX), 0u);
}

TEST_F(JitExecTest, ShortRepStosdMatchesInterpreter) {
    m_cpu->gpr.set32(EAX, 0xA5A5A5A5u);
    m_cpu->gpr.set32(EDI, 0x3000);
    m_cpu->gpr.set32(ECX, 3);
    const u8 code[] = {0xF3, 0xAB};  // REP STOSD

    CpuState icpu = *m_cpu;
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    std::array<u8, 12> before{};
    ASSERT_TRUE(m_memory->read_bytes(0x3000, before.data(), before.size()));
    {
        Interpreter interp(icpu, *m_memory);
        EXPECT_EQ(interp.run(1), ExecResult::Continue);
    }
    std::array<u8, 12> interp_dst{};
    ASSERT_TRUE(m_memory->read_bytes(0x3000, interp_dst.data(), interp_dst.size()));
    ASSERT_TRUE(m_memory->write_bytes(0x3000, before.data(), before.size()));

    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    std::array<u8, 12> jit_dst{};
    ASSERT_TRUE(m_memory->read_bytes(0x3000, jit_dst.data(), jit_dst.size()));
    EXPECT_EQ(jit_dst, interp_dst);
    EXPECT_EQ(m_cpu->gpr.get32(EDI), icpu.gpr.get32(EDI));
    EXPECT_EQ(m_cpu->gpr.get32(ECX), 0u);
}

// VITREO_JIT_SHORT_REP is the immediate of one CMP. A value past the 12-bit
// field is clamped to 4095 (with one log line), never masked to CMP #0 and
// never silently replaced by the default 64. Parsed once per process, so ctest
// runs this as vitreo_cpu_tests_short_rep_knob_overflow
// (VITREO_JIT_SHORT_REP=4096); it skips elsewhere.
TEST_F(JitExecTest, ShortRepKnobPastTheCmpFieldClampsTo4095) {
    const char* knob = std::getenv("VITREO_JIT_SHORT_REP");
    if (knob == nullptr || std::strcmp(knob, "4096") != 0) {
        GTEST_SKIP() << "runs as vitreo_cpu_tests_short_rep_knob_overflow";
    }
    const u8 code[] = {0xF3, 0xA4};  // REP MOVSB
    BasicBlock block = build_block(code, sizeof(code));
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);
    const auto* words = reinterpret_cast<const u32*>(cb->code);
    // CMP Wn, #imm (SUBS WZR, Wn, #imm), any Wn.
    const auto is_cmp_w = [](u32 word, u32 imm) {
        return (word & ~(0x1Fu << 5)) == arm64::cmp_w_imm(0, imm);
    };
    bool clamped = false;
    for (u32 i = 0; i < cb->code_size / 4; ++i) {
        EXPECT_FALSE(is_cmp_w(words[i], 64)) << "dropped to the default 64";
        clamped = clamped || is_cmp_w(words[i], 4095);
    }
    EXPECT_TRUE(clamped) << "the inline short-REP bound must be CMP #4095";
}
