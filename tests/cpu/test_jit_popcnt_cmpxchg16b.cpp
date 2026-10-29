// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT execution tests — POPCNT and (LOCK) CMPXCHG16B

#include "jit_exec_test_fixture.hpp"

// ============================================================================
// POPCNT r16/r64 (Intel SDM Vol. 2 POPCNT)
// ============================================================================

TEST_F(JitExecTest, PopcntR16MatchesInterpreter) {
    // 66 F3 0F B8 C1 = POPCNT AX, CX
    const u8 code[] = {0x66, 0xF3, 0x0F, 0xB8, 0xC1};
    m_cpu->gpr.set32(ECX, 0x0000F0F0u);
    m_cpu->gpr.set32(EAX, 0xAABBCCDDu);

    CpuState icpu = *m_cpu;
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    {
        Interpreter interp(icpu, *m_memory);
        EXPECT_EQ(interp.run(1), ExecResult::Continue);
    }
    icpu.materialize_lazy_flags();

    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    m_cpu->materialize_lazy_flags();

    EXPECT_EQ(m_cpu->gpr.get16(EAX), 8u);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0xAABB0008u);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), icpu.gpr.get32(EAX));
    EXPECT_FALSE(m_cpu->flags.zf());
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.of());
    EXPECT_FALSE(m_cpu->flags.sf());
    EXPECT_FALSE(m_cpu->flags.pf());
    EXPECT_EQ(m_cpu->flags.zf(), icpu.flags.zf());
    EXPECT_EQ(m_cpu->flags.cf(), icpu.flags.cf());
}

TEST_F(JitExecTest, PopcntR16ZeroSetsZf) {
    const u8 code[] = {0x66, 0xF3, 0x0F, 0xB8, 0xC1};
    m_cpu->gpr.set32(ECX, 0xFFFF0000u);  // CX == 0
    m_cpu->gpr.set32(EAX, 0x11111111u);
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    m_cpu->materialize_lazy_flags();
    EXPECT_EQ(m_cpu->gpr.get16(EAX), 0u);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0x11110000u);
    EXPECT_TRUE(m_cpu->flags.zf());
    EXPECT_FALSE(m_cpu->flags.cf());
}

TEST_F(JitExecTest, PopcntR64MatchesInterpreter) {
    // F3 48 0F B8 C1 = POPCNT RAX, RCX
    const u8 code[] = {0xF3, 0x48, 0x0F, 0xB8, 0xC1};
    m_cpu->init_long_mode();
    m_cpu->gpr.set64(ESP, 0x10000);
    m_cpu->rip = 0x1000;
    m_cpu->gpr.set64(ECX, 0xF0F0F0F0F0F0F0F0ull);

    CpuState icpu = *m_cpu;
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    {
        Interpreter interp(icpu, *m_memory);
        EXPECT_EQ(interp.run(1), ExecResult::Continue);
    }
    icpu.materialize_lazy_flags();

    EXPECT_EQ(jit_run64(code, sizeof(code)), JitExecResult::Continue);
    m_cpu->materialize_lazy_flags();

    EXPECT_EQ(m_cpu->gpr.get64(EAX), 32u);
    EXPECT_EQ(m_cpu->gpr.get64(EAX), icpu.gpr.get64(EAX));
    EXPECT_FALSE(m_cpu->flags.zf());
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.of());
    EXPECT_FALSE(m_cpu->flags.sf());
    EXPECT_FALSE(m_cpu->flags.pf());
    EXPECT_EQ(m_cpu->flags.zf(), icpu.flags.zf());
    EXPECT_EQ(m_cpu->flags.cf(), icpu.flags.cf());
}

// ============================================================================
// CMPXCHG16B m128 (Intel SDM Vol. 2 CMPXCHG16B)
// ============================================================================

TEST_F(JitExecTest, Cmpxchg16bEqual) {
    // 48 0F C7 0E = CMPXCHG16B [RSI]
    const u8 code[] = {0x48, 0x0F, 0xC7, 0x0E};
    const GuestAddr m128 = 0x2000;
    const u64 mem_lo = 0x1111111111111111ull;
    const u64 mem_hi = 0x2222222222222222ull;
    const u64 rbx_v = 0x3333333333333333ull;
    const u64 rcx_v = 0x4444444444444444ull;

    auto setup = [&](CpuState& cpu) {
        cpu.init_long_mode();
        cpu.gpr.set64(ESP, 0x10000);
        cpu.rip = 0x1000;
        cpu.gpr.set64(ESI, m128);
        cpu.gpr.set64(EAX, mem_lo);
        cpu.gpr.set64(EDX, mem_hi);
        cpu.gpr.set64(EBX, rbx_v);
        cpu.gpr.set64(ECX, rcx_v);
    };

    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    ASSERT_TRUE(m_memory->write_u64(m128, mem_lo));
    ASSERT_TRUE(m_memory->write_u64(m128 + 8, mem_hi));

    CpuState icpu;
    setup(icpu);
    {
        Interpreter interp(icpu, *m_memory);
        EXPECT_EQ(interp.run(1), ExecResult::Continue);
    }
    icpu.materialize_lazy_flags();
    auto i_lo = m_memory->read_u64(m128);
    auto i_hi = m_memory->read_u64(m128 + 8);
    ASSERT_TRUE(i_lo.is_ok() && i_hi.is_ok());

    ASSERT_TRUE(m_memory->write_u64(m128, mem_lo));
    ASSERT_TRUE(m_memory->write_u64(m128 + 8, mem_hi));
    setup(*m_cpu);
    EXPECT_EQ(jit_run64(code, sizeof(code)), JitExecResult::Continue);
    m_cpu->materialize_lazy_flags();
    auto j_lo = m_memory->read_u64(m128);
    auto j_hi = m_memory->read_u64(m128 + 8);
    ASSERT_TRUE(j_lo.is_ok() && j_hi.is_ok());

    EXPECT_TRUE(icpu.flags.zf());
    EXPECT_TRUE(m_cpu->flags.zf());
    EXPECT_EQ(j_lo.value(), rbx_v);
    EXPECT_EQ(j_hi.value(), rcx_v);
    EXPECT_EQ(i_lo.value(), j_lo.value());
    EXPECT_EQ(i_hi.value(), j_hi.value());
    EXPECT_EQ(m_cpu->gpr.get64(EAX), icpu.gpr.get64(EAX));
    EXPECT_EQ(m_cpu->gpr.get64(EDX), icpu.gpr.get64(EDX));
}

TEST_F(JitExecTest, Cmpxchg16bNotEqual) {
    const u8 code[] = {0x48, 0x0F, 0xC7, 0x0E};
    const GuestAddr m128 = 0x2000;
    const u64 mem_lo = 0xAAAAAAAAAAAAAAAAULL;
    const u64 mem_hi = 0xBBBBBBBBBBBBBBBBULL;

    auto setup = [&](CpuState& cpu) {
        cpu.init_long_mode();
        cpu.gpr.set64(ESP, 0x10000);
        cpu.rip = 0x1000;
        cpu.gpr.set64(ESI, m128);
        cpu.gpr.set64(EAX, 1);
        cpu.gpr.set64(EDX, 2);
        cpu.gpr.set64(EBX, 3);
        cpu.gpr.set64(ECX, 4);
    };

    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    ASSERT_TRUE(m_memory->write_u64(m128, mem_lo));
    ASSERT_TRUE(m_memory->write_u64(m128 + 8, mem_hi));

    CpuState icpu;
    setup(icpu);
    {
        Interpreter interp(icpu, *m_memory);
        EXPECT_EQ(interp.run(1), ExecResult::Continue);
    }
    icpu.materialize_lazy_flags();

    ASSERT_TRUE(m_memory->write_u64(m128, mem_lo));
    ASSERT_TRUE(m_memory->write_u64(m128 + 8, mem_hi));
    setup(*m_cpu);
    EXPECT_EQ(jit_run64(code, sizeof(code)), JitExecResult::Continue);
    m_cpu->materialize_lazy_flags();

    EXPECT_FALSE(icpu.flags.zf());
    EXPECT_FALSE(m_cpu->flags.zf());
    EXPECT_EQ(m_cpu->gpr.get64(EAX), mem_lo);
    EXPECT_EQ(m_cpu->gpr.get64(EDX), mem_hi);
    EXPECT_EQ(m_cpu->gpr.get64(EAX), icpu.gpr.get64(EAX));
    EXPECT_EQ(m_cpu->gpr.get64(EDX), icpu.gpr.get64(EDX));
    auto j_lo = m_memory->read_u64(m128);
    auto j_hi = m_memory->read_u64(m128 + 8);
    ASSERT_TRUE(j_lo.is_ok() && j_hi.is_ok());
    EXPECT_EQ(j_lo.value(), mem_lo);
    EXPECT_EQ(j_hi.value(), mem_hi);
}

TEST_F(JitExecTest, Cmpxchg16bUnalignedRaisesGp) {
    const u8 code[] = {0x48, 0x0F, 0xC7, 0x0E};
    m_cpu->init_long_mode();
    m_cpu->gpr.set64(ESP, 0x10000);
    m_cpu->rip = 0x1000;
    m_cpu->gpr.set64(ESI, 0x2001);  // not 16-byte aligned

    CpuState icpu = *m_cpu;
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    ExecResult ir;
    {
        Interpreter interp(icpu, *m_memory);
        ir = interp.run(1);
    }
    EXPECT_EQ(ir, ExecResult::Exception);

    EXPECT_EQ(jit_run64(code, sizeof(code)), JitExecResult::Continue);
    ASSERT_TRUE(m_cpu->pending_raise.active);
    EXPECT_TRUE(m_cpu->pending_raise.from_helper);
    EXPECT_EQ(m_cpu->pending_raise.code, 0xC0000005u);
}

// ============================================================================
// LOCK CMPXCHG16B m128 (Intel SDM Vol. 2 CMPXCHG16B + LOCK)
// Encoding: F0 48 0F C7 /1
// ============================================================================

TEST_F(JitExecTest, LockCmpxchg16bEqual) {
    // F0 48 0F C7 0E = LOCK CMPXCHG16B [RSI]
    const u8 code[] = {0xF0, 0x48, 0x0F, 0xC7, 0x0E};
    const GuestAddr m128 = 0x2000;
    const u64 mem_lo = 0x1111111111111111ull;
    const u64 mem_hi = 0x2222222222222222ull;
    const u64 rbx_v = 0x3333333333333333ull;
    const u64 rcx_v = 0x4444444444444444ull;

    auto setup = [&](CpuState& cpu) {
        cpu.init_long_mode();
        cpu.gpr.set64(ESP, 0x10000);
        cpu.rip = 0x1000;
        cpu.gpr.set64(ESI, m128);
        cpu.gpr.set64(EAX, mem_lo);
        cpu.gpr.set64(EDX, mem_hi);
        cpu.gpr.set64(EBX, rbx_v);
        cpu.gpr.set64(ECX, rcx_v);
    };

    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    ASSERT_TRUE(m_memory->write_u64(m128, mem_lo));
    ASSERT_TRUE(m_memory->write_u64(m128 + 8, mem_hi));

    CpuState icpu;
    setup(icpu);
    {
        Interpreter interp(icpu, *m_memory);
        EXPECT_EQ(interp.run(1), ExecResult::Continue);
    }
    icpu.materialize_lazy_flags();
    auto i_lo = m_memory->read_u64(m128);
    auto i_hi = m_memory->read_u64(m128 + 8);
    ASSERT_TRUE(i_lo.is_ok() && i_hi.is_ok());

    ASSERT_TRUE(m_memory->write_u64(m128, mem_lo));
    ASSERT_TRUE(m_memory->write_u64(m128 + 8, mem_hi));
    setup(*m_cpu);
    EXPECT_EQ(jit_run64(code, sizeof(code)), JitExecResult::Continue);
    m_cpu->materialize_lazy_flags();
    auto j_lo = m_memory->read_u64(m128);
    auto j_hi = m_memory->read_u64(m128 + 8);
    ASSERT_TRUE(j_lo.is_ok() && j_hi.is_ok());

    EXPECT_TRUE(icpu.flags.zf());
    EXPECT_TRUE(m_cpu->flags.zf());
    EXPECT_EQ(j_lo.value(), rbx_v);
    EXPECT_EQ(j_hi.value(), rcx_v);
    EXPECT_EQ(i_lo.value(), j_lo.value());
    EXPECT_EQ(i_hi.value(), j_hi.value());
    EXPECT_EQ(m_cpu->gpr.get64(EAX), icpu.gpr.get64(EAX));
    EXPECT_EQ(m_cpu->gpr.get64(EDX), icpu.gpr.get64(EDX));
}

TEST_F(JitExecTest, LockCmpxchg16bNotEqual) {
    const u8 code[] = {0xF0, 0x48, 0x0F, 0xC7, 0x0E};
    const GuestAddr m128 = 0x2000;
    const u64 mem_lo = 0xAAAAAAAAAAAAAAAAULL;
    const u64 mem_hi = 0xBBBBBBBBBBBBBBBBULL;

    auto setup = [&](CpuState& cpu) {
        cpu.init_long_mode();
        cpu.gpr.set64(ESP, 0x10000);
        cpu.rip = 0x1000;
        cpu.gpr.set64(ESI, m128);
        cpu.gpr.set64(EAX, 1);
        cpu.gpr.set64(EDX, 2);
        cpu.gpr.set64(EBX, 3);
        cpu.gpr.set64(ECX, 4);
    };

    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    ASSERT_TRUE(m_memory->write_u64(m128, mem_lo));
    ASSERT_TRUE(m_memory->write_u64(m128 + 8, mem_hi));

    CpuState icpu;
    setup(icpu);
    {
        Interpreter interp(icpu, *m_memory);
        EXPECT_EQ(interp.run(1), ExecResult::Continue);
    }
    icpu.materialize_lazy_flags();

    ASSERT_TRUE(m_memory->write_u64(m128, mem_lo));
    ASSERT_TRUE(m_memory->write_u64(m128 + 8, mem_hi));
    setup(*m_cpu);
    EXPECT_EQ(jit_run64(code, sizeof(code)), JitExecResult::Continue);
    m_cpu->materialize_lazy_flags();

    EXPECT_FALSE(icpu.flags.zf());
    EXPECT_FALSE(m_cpu->flags.zf());
    EXPECT_EQ(m_cpu->gpr.get64(EAX), mem_lo);
    EXPECT_EQ(m_cpu->gpr.get64(EDX), mem_hi);
    EXPECT_EQ(m_cpu->gpr.get64(EAX), icpu.gpr.get64(EAX));
    EXPECT_EQ(m_cpu->gpr.get64(EDX), icpu.gpr.get64(EDX));
    auto j_lo = m_memory->read_u64(m128);
    auto j_hi = m_memory->read_u64(m128 + 8);
    ASSERT_TRUE(j_lo.is_ok() && j_hi.is_ok());
    EXPECT_EQ(j_lo.value(), mem_lo);
    EXPECT_EQ(j_hi.value(), mem_hi);
}

TEST_F(JitExecTest, LockCmpxchg16bUnalignedRaisesGp) {
    const u8 code[] = {0xF0, 0x48, 0x0F, 0xC7, 0x0E};
    m_cpu->init_long_mode();
    m_cpu->gpr.set64(ESP, 0x10000);
    m_cpu->rip = 0x1000;
    m_cpu->gpr.set64(ESI, 0x2001);  // not 16-byte aligned

    CpuState icpu = *m_cpu;
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    ExecResult ir;
    {
        Interpreter interp(icpu, *m_memory);
        ir = interp.run(1);
    }
    EXPECT_EQ(ir, ExecResult::Exception);

    EXPECT_EQ(jit_run64(code, sizeof(code)), JitExecResult::Continue);
    ASSERT_TRUE(m_cpu->pending_raise.active);
    EXPECT_TRUE(m_cpu->pending_raise.from_helper);
    EXPECT_EQ(m_cpu->pending_raise.code, 0xC0000005u);
}
