// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT execution tests — ESP/EBP frame addressing and pinned-register indirection

#include "jit_exec_test_fixture.hpp"

TEST_F(JitExecTest, EspEbpDispBlockMatchesInterpreter) {
    // 32-bit [esp+disp] / [ebp+disp] must use the pinned ESP/EBP EA path
    // and match the interpreter — including a disp that does not fit uimm12.
    const u8 code[] = {
        0x8B, 0x44, 0x24, 0x04,             // mov eax, [esp+4]
        0x8B, 0x4D, 0x08,                   // mov ecx, [ebp+8]
        0x8B, 0x96, 0x00, 0x10, 0x00, 0x00, // mov edx, [esi+0x1000]
        0x01, 0xC8,                         // add eax, ecx
        0x01, 0xD0,                         // add eax, edx
    };

    m_cpu->gpr.set32(ESP, 0x4000);
    m_cpu->gpr.set32(EBP, 0x5000);
    m_cpu->gpr.set32(ESI, 0x2000);
    ASSERT_TRUE(m_memory->write_u32(0x4004, 0x11111111u));
    ASSERT_TRUE(m_memory->write_u32(0x5008, 0x22222222u));
    ASSERT_TRUE(m_memory->write_u32(0x3000, 0x00000010u));

    CpuState icpu = *m_cpu;
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    {
        Interpreter interp(icpu, *m_memory);
        EXPECT_EQ(interp.run(5), ExecResult::Continue);
    }

    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), icpu.gpr.get32(EAX));
    EXPECT_EQ(m_cpu->gpr.get32(ECX), icpu.gpr.get32(ECX));
    EXPECT_EQ(m_cpu->gpr.get32(EDX), icpu.gpr.get32(EDX));
    EXPECT_EQ(m_cpu->gpr.get32(ESP), icpu.gpr.get32(ESP));
    EXPECT_EQ(m_cpu->gpr.get32(EBP), icpu.gpr.get32(EBP));
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0x33333343u);
}

TEST_F(JitExecTest, EspEdxScaledIndexMatchesInterpreter) {
    // [esp+edx*8+0x28] — pinned base+index ADD Wd, Wsp, Wdx, LSL #3.
    const u8 code[] = {
        0x8B, 0x44, 0xD4, 0x28,  // mov eax, [esp+edx*8+0x28]
        0x8D, 0x4C, 0xD4, 0x28,  // lea ecx, [esp+edx*8+0x28]
    };
    m_cpu->gpr.set32(ESP, 0x4000);
    m_cpu->gpr.set32(EDX, 3);
    ASSERT_TRUE(m_memory->write_u32(0x4000 + 3 * 8 + 0x28, 0xA1B2C3D4u));

    CpuState icpu = *m_cpu;
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    {
        Interpreter interp(icpu, *m_memory);
        EXPECT_EQ(interp.run(2), ExecResult::Continue);
    }

    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), icpu.gpr.get32(EAX));
    EXPECT_EQ(m_cpu->gpr.get32(ECX), icpu.gpr.get32(ECX));
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0xA1B2C3D4u);
    EXPECT_EQ(m_cpu->gpr.get32(ECX), 0x4000u + 3u * 8u + 0x28u);
}

TEST_F(JitExecTest, PinRegIndirectLoadStoreMatchesInterpreter) {
    // [edx] / [ebx] must LDR/STR through the live pin, not a scratch copy.
    const u8 code[] = {
        0x8B, 0x02,        // mov eax, [edx]
        0x89, 0x03,        // mov [ebx], eax
        0x03, 0x32,        // add esi, [edx]
    };
    m_cpu->gpr.set32(EDX, 0x3000);
    m_cpu->gpr.set32(EBX, 0x3010);
    m_cpu->gpr.set32(ESI, 0x10);
    ASSERT_TRUE(m_memory->write_u32(0x3000, 0xAABBCCDDu));
    ASSERT_TRUE(m_memory->write_u32(0x3010, 0));

    CpuState icpu = *m_cpu;
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    {
        Interpreter interp(icpu, *m_memory);
        EXPECT_EQ(interp.run(3), ExecResult::Continue);
    }
    const u32 i_ebx_mem = m_memory->read_u32(0x3010).value_or(0);
    ASSERT_TRUE(m_memory->write_u32(0x3010, 0));

    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), icpu.gpr.get32(EAX));
    EXPECT_EQ(m_cpu->gpr.get32(ESI), icpu.gpr.get32(ESI));
    EXPECT_EQ(m_memory->read_u32(0x3010).value_or(0), i_ebx_mem);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0xAABBCCDDu);
    EXPECT_EQ(m_cpu->gpr.get32(ESI), 0xAABBCCEDu);
}

TEST_F(JitExecTest, EspDispClusterAndRmwMatchesInterpreter) {
    // Several [esp+disp] loads share one linear+ESP frame; RMW stores once.
    const u8 code[] = {
        0x8B, 0x44, 0x24, 0x18,             // mov eax, [esp+0x18]
        0x3B, 0x4C, 0x24, 0x1C,             // cmp ecx, [esp+0x1C]
        0x83, 0x6C, 0x24, 0x14, 0x04,       // sub dword [esp+0x14], 4
        0xFF, 0x44, 0x24, 0x14,             // inc dword [esp+0x14]
    };
    m_cpu->gpr.set32(ESP, 0x4000);
    m_cpu->gpr.set32(ECX, 0x20);
    m_cpu->flags.set_cf(true);
    ASSERT_TRUE(m_memory->write_u32(0x4018, 0x11111111u));
    ASSERT_TRUE(m_memory->write_u32(0x401C, 0x20));
    ASSERT_TRUE(m_memory->write_u32(0x4014, 0x02));

    CpuState icpu = *m_cpu;
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    {
        Interpreter interp(icpu, *m_memory);
        EXPECT_EQ(interp.run(4), ExecResult::Continue);
    }
    const u32 i_slot = m_memory->read_u32(0x4014).value_or(0);
    ASSERT_TRUE(m_memory->write_u32(0x4014, 0x02));

    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), icpu.gpr.get32(EAX));
    EXPECT_EQ(m_memory->read_u32(0x4014).value_or(0), i_slot);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0x11111111u);
    EXPECT_EQ(m_memory->read_u32(0x4014).value_or(0), 0xFFFFFFFFu);
}

TEST_F(JitExecTest, EspDispStoreProbeStillWrites) {
    // [esp+disp] store keeps the SMC/span probe; value must land and
    // a later load must see it through the same frame.
    const u8 code[] = {
        0xC7, 0x44, 0x24, 0x08, 0x78, 0x56, 0x34, 0x12, // mov [esp+8], 0x12345678
        0x8B, 0x44, 0x24, 0x08,                         // mov eax, [esp+8]
        0x83, 0x44, 0x24, 0x08, 0x01,                   // add dword [esp+8], 1
        0x8B, 0x4C, 0x24, 0x08,                         // mov ecx, [esp+8]
    };
    m_cpu->gpr.set32(ESP, 0x4000);
    ASSERT_TRUE(m_memory->write_u32(0x4008, 0));

    CpuState icpu = *m_cpu;
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    {
        Interpreter interp(icpu, *m_memory);
        EXPECT_EQ(interp.run(4), ExecResult::Continue);
    }
    const u32 i_slot = m_memory->read_u32(0x4008).value_or(0);
    ASSERT_TRUE(m_memory->write_u32(0x4008, 0));

    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), icpu.gpr.get32(EAX));
    EXPECT_EQ(m_cpu->gpr.get32(ECX), icpu.gpr.get32(ECX));
    EXPECT_EQ(m_memory->read_u32(0x4008).value_or(0), i_slot);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0x12345678u);
    EXPECT_EQ(m_cpu->gpr.get32(ECX), 0x12345679u);
}

TEST_F(JitExecTest, EspWriteKillsFrameThenReloads) {
    // ADD ESP must drop the host frame so the next [esp+disp] uses new ESP.
    const u8 code[] = {
        0x8B, 0x44, 0x24, 0x04,             // mov eax, [esp+4]
        0x83, 0xC4, 0x10,                   // add esp, 0x10
        0x8B, 0x4C, 0x24, 0x04,             // mov ecx, [esp+4]
    };
    m_cpu->gpr.set32(ESP, 0x4000);
    ASSERT_TRUE(m_memory->write_u32(0x4004, 0xAAAAAAAAu));
    ASSERT_TRUE(m_memory->write_u32(0x4014, 0xBBBBBBBBu));

    CpuState icpu = *m_cpu;
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    {
        Interpreter interp(icpu, *m_memory);
        EXPECT_EQ(interp.run(3), ExecResult::Continue);
    }

    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), icpu.gpr.get32(EAX));
    EXPECT_EQ(m_cpu->gpr.get32(ECX), icpu.gpr.get32(ECX));
    EXPECT_EQ(m_cpu->gpr.get32(ESP), icpu.gpr.get32(ESP));
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0xAAAAAAAAu);
    EXPECT_EQ(m_cpu->gpr.get32(ECX), 0xBBBBBBBBu);
    EXPECT_EQ(m_cpu->gpr.get32(ESP), 0x4010u);
}
