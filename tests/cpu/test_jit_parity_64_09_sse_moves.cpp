// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests — 64-bit long mode (CPU-048): SSE 64-bit-lane
// loads and MOVDQU/MOVDQA/PSHUFD.

#include "jit_parity_64_fixture.hpp"

// ============================================================================
// SSE 64-bit-lane memory loads inside a 64-bit block (native codegen added for
// the Steam-extraction hot path). The XMM result is compared (check_xmm=true);
// the unwritten lane must be preserved. Source bytes live at 0x2000.
// ============================================================================

TEST_F(JitParity64Test, MovlpdXmmM64) {
    // MOVLPD XMM0, [RAX] — 66 0F 12 00 (low lane = [mem], high preserved)
    u8 src[8] = {0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88};
    ASSERT_TRUE(m_memory->write_bytes(0x2000, src, 8));
    check_parity({0x66, 0x0F, 0x12, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x2000);
            cpu.sse.xmm[0].u64_[0] = 0xDEADDEADDEADDEADull;
            cpu.sse.xmm[0].u64_[1] = 0xCAFECAFECAFECAFEull; // must survive
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParity64Test, MovlpsXmmM64) {
    // MOVLPS XMM0, [RAX] — 0F 12 00
    u8 src[8] = {0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08};
    ASSERT_TRUE(m_memory->write_bytes(0x2000, src, 8));
    check_parity({0x0F, 0x12, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x2000);
            cpu.sse.xmm[0].u64_[1] = 0x1122334455667788ull;
        },
        false, true);
}

TEST_F(JitParity64Test, MovhpdXmmM64) {
    // MOVHPD XMM0, [RAX] — 66 0F 16 00 (high lane = [mem], low preserved)
    u8 src[8] = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x00,0x99};
    ASSERT_TRUE(m_memory->write_bytes(0x2000, src, 8));
    check_parity({0x66, 0x0F, 0x16, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x2000);
            cpu.sse.xmm[0].u64_[0] = 0x0123456789ABCDEFull; // must survive
        },
        false, true);
}

TEST_F(JitParity64Test, MovhpsXmmM64) {
    // MOVHPS XMM0, [RAX] — 0F 16 00
    u8 src[8] = {0xDE,0xAD,0xBE,0xEF,0xFE,0xED,0xFA,0xCE};
    ASSERT_TRUE(m_memory->write_bytes(0x2000, src, 8));
    check_parity({0x0F, 0x16, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x2000);
            cpu.sse.xmm[0].u64_[0] = 0xF00DF00DF00DF00Dull;
        },
        false, true);
}

// ----- MOVUPS/MOVAPS/MOVUPD/MOVAPD: 128-bit moves (native; MOVUPS store was
// the dominant Steam CEF-init JIT fallback) -----

TEST_F(JitParity64Test, MovupsLoadMem) {
    // MOVUPS XMM0, [RAX] — 0F 10 00
    u8 src[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    ASSERT_TRUE(m_memory->write_bytes(0x2000, src, 16));
    check_parity({0x0F, 0x10, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x2000);
            cpu.sse.xmm[0].u64_[0] = 0; cpu.sse.xmm[0].u64_[1] = 0;
        },
        false, true);
}

TEST_F(JitParity64Test, MovupdLoadMem) {
    // MOVUPD XMM0, [RAX] — 66 0F 10 00
    u8 src[16] = {0xFF,0xEE,0xDD,0xCC,0xBB,0xAA,0x99,0x88,
                  0x77,0x66,0x55,0x44,0x33,0x22,0x11,0x00};
    ASSERT_TRUE(m_memory->write_bytes(0x2000, src, 16));
    check_parity({0x66, 0x0F, 0x10, 0x00},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0x2000); },
        false, true);
}

TEST_F(JitParity64Test, MovapsRegReg) {
    // MOVAPS XMM0, XMM1 — 0F 28 C1
    check_parity({0x0F, 0x28, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[1].u64_[0] = 0xA1A2A3A4A5A6A7A8ull;
            cpu.sse.xmm[1].u64_[1] = 0xB1B2B3B4B5B6B7B8ull;
        },
        false, true);
}

TEST_F(JitParity64Test, NativeMovapsAlignedLoadStore) {
    u8 src[16] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
    };
    ASSERT_TRUE(m_memory->write_bytes(0x2040, src, sizeof(src)));
    // MOVAPS XMM8,[RAX]; MOVAPS [RAX+0x10],XMM8.
    check_native_parity({
            0x44, 0x0F, 0x28, 0x00,
            0x44, 0x0F, 0x29, 0x40, 0x10,
        },
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0x2040); },
        true, true);
    u8 out[16] = {};
    ASSERT_TRUE(m_memory->read_bytes(0x2050, out, sizeof(out)).is_ok());
    EXPECT_EQ(std::memcmp(out, src, sizeof(src)), 0);
}

TEST_F(JitParity64Test, NativeMovapsMisalignedLoadRaisesGp) {
    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    const u8 code[] = {0x0F, 0x28, 0x00};  // MOVAPS XMM0,[RAX]
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    BasicBlock block = build_block(code, sizeof(code));
    ASSERT_EQ(block.instruction_count, 1u);

    CpuState cpu;
    init_cpu(cpu);
    cpu.gpr.set64(RAX, 0x2041);
    cpu.sse.xmm[0].u64_[0] = 0xAAAAAAAAAAAAAAAAull;
    cpu.sse.xmm[0].u64_[1] = 0xBBBBBBBBBBBBBBBBull;
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    EXPECT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    EXPECT_EQ(cpu.sse.xmm[0].u64_[0], 0xAAAAAAAAAAAAAAAAull);
    EXPECT_EQ(cpu.sse.xmm[0].u64_[1], 0xBBBBBBBBBBBBBBBBull);
}

TEST_F(JitParity64Test, NativeMovapdAlignedLoadStore) {
    u8 src[16] = {
        0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE,
        0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01,
    };
    ASSERT_TRUE(m_memory->write_bytes(0x2040, src, sizeof(src)));
    // MOVAPD XMM8,[RAX]; MOVAPD [RAX+0x10],XMM8.
    check_native_parity({
            0x66, 0x44, 0x0F, 0x28, 0x00,
            0x66, 0x44, 0x0F, 0x29, 0x40, 0x10,
        },
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0x2040); },
        true, true);
    u8 out[16] = {};
    ASSERT_TRUE(m_memory->read_bytes(0x2050, out, sizeof(out)).is_ok());
    EXPECT_EQ(std::memcmp(out, src, sizeof(src)), 0);
}

TEST_F(JitParity64Test, NativeMovapdMisalignedStoreRaisesGp) {
    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    const u8 code[] = {0x66, 0x0F, 0x29, 0x00};  // MOVAPD [RAX],XMM0
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    BasicBlock block = build_block(code, sizeof(code));
    ASSERT_EQ(block.instruction_count, 1u);

    CpuState cpu;
    init_cpu(cpu);
    cpu.gpr.set64(RAX, 0x2041);
    cpu.sse.xmm[0].u64_[0] = 0x0123456789ABCDEFull;
    cpu.sse.xmm[0].u64_[1] = 0xFEDCBA9876543210ull;
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    EXPECT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    u8 out[16] = {};
    ASSERT_TRUE(m_memory->read_bytes(0x2041, out, sizeof(out)).is_ok());
    for (u8 byte : out) EXPECT_EQ(byte, 0);
}

TEST_F(JitParity64Test, MovupsStoreReloadRoundtrip) {
    // MOVUPS [RAX], XMM1 (0F 11 08) ; MOVUPS XMM0, [RAX] (0F 10 00)
    // Verifies the native unaligned 128-bit memory STORE: xmm1 -> mem -> xmm0.
    check_parity({0x0F, 0x11, 0x08, 0x0F, 0x10, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x2000);
            cpu.sse.xmm[1].u64_[0] = 0xCAFEBABEDEADBEEFull;
            cpu.sse.xmm[1].u64_[1] = 0x0011223344556677ull;
            cpu.sse.xmm[0].u64_[0] = 0; cpu.sse.xmm[0].u64_[1] = 0;
        },
        false, true);
}

// ----- BLENDPD: register source -----

TEST_F(JitParity64Test, BlendpdRegLane0) {
    // BLENDPD XMM0, XMM1, 0x1 — 66 0F 3A 0D C1 01 (lane0 from src)
    check_parity({0x66, 0x0F, 0x3A, 0x0D, 0xC1, 0x01},
        [](CpuState& cpu) {
            cpu.sse.xmm[0].u64_[0] = 0x1111111111111111ull;
            cpu.sse.xmm[0].u64_[1] = 0x2222222222222222ull;
            cpu.sse.xmm[1].u64_[0] = 0xAAAAAAAAAAAAAAAAull;
            cpu.sse.xmm[1].u64_[1] = 0xBBBBBBBBBBBBBBBBull;
        },
        false, true);
}

TEST_F(JitParity64Test, BlendpdRegLane1) {
    // BLENDPD XMM0, XMM1, 0x2 — lane1 from src
    check_parity({0x66, 0x0F, 0x3A, 0x0D, 0xC1, 0x02},
        [](CpuState& cpu) {
            cpu.sse.xmm[0].u64_[0] = 0x1111111111111111ull;
            cpu.sse.xmm[0].u64_[1] = 0x2222222222222222ull;
            cpu.sse.xmm[1].u64_[0] = 0xAAAAAAAAAAAAAAAAull;
            cpu.sse.xmm[1].u64_[1] = 0xBBBBBBBBBBBBBBBBull;
        },
        false, true);
}

TEST_F(JitParity64Test, BlendpdRegBoth) {
    // BLENDPD XMM0, XMM1, 0x3 — both lanes from src
    check_parity({0x66, 0x0F, 0x3A, 0x0D, 0xC1, 0x03},
        [](CpuState& cpu) {
            cpu.sse.xmm[0].u64_[0] = 0x1111111111111111ull;
            cpu.sse.xmm[0].u64_[1] = 0x2222222222222222ull;
            cpu.sse.xmm[1].u64_[0] = 0xAAAAAAAAAAAAAAAAull;
            cpu.sse.xmm[1].u64_[1] = 0xBBBBBBBBBBBBBBBBull;
        },
        false, true);
}

// ----- BLENDPD: memory source (exercises EA and EA+8) -----

TEST_F(JitParity64Test, BlendpdMemBoth) {
    // BLENDPD XMM0, [RAX], 0x3 — 66 0F 3A 0D 00 03
    u8 src[16] = {0,1,2,3,4,5,6,7, 8,9,10,11,12,13,14,15};
    ASSERT_TRUE(m_memory->write_bytes(0x2000, src, 16));
    check_parity({0x66, 0x0F, 0x3A, 0x0D, 0x00, 0x03},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x2000);
            cpu.sse.xmm[0].u64_[0] = 0x9999999999999999ull;
            cpu.sse.xmm[0].u64_[1] = 0x8888888888888888ull;
        },
        false, true);
}

TEST_F(JitParity64Test, BlendpdMemHighLane) {
    // BLENDPD XMM0, [RAX], 0x2 — high lane from [RAX+8], low preserved
    u8 src[16] = {0,1,2,3,4,5,6,7, 0xF0,0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7};
    ASSERT_TRUE(m_memory->write_bytes(0x2000, src, 16));
    check_parity({0x66, 0x0F, 0x3A, 0x0D, 0x00, 0x02},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x2000);
            cpu.sse.xmm[0].u64_[0] = 0x7777777777777777ull; // must survive
            cpu.sse.xmm[0].u64_[1] = 0x6666666666666666ull;
        },
        false, true);
}

// ============================================================================
// MOVDQU/MOVDQA/PSHUFD — the actual top Steam-extraction SSE fallbacks.
// (Run these under VITREO_JIT_NO_DELEGATE=1 to confirm the NATIVE path is
// taken, not the interpreter delegate.)
// ============================================================================

TEST_F(JitParity64Test, MovdquLoadReg) {
    // MOVDQU XMM0, XMM1 — F3 0F 6F C1
    check_parity({0xF3, 0x0F, 0x6F, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[1].u64_[0] = 0x0123456789ABCDEFull;
            cpu.sse.xmm[1].u64_[1] = 0xFEDCBA9876543210ull;
        },
        false, true);
}

TEST_F(JitParity64Test, MovdquLoadMem) {
    // MOVDQU XMM0, [RAX] — F3 0F 6F 00 (unaligned 16-byte load)
    u8 src[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    ASSERT_TRUE(m_memory->write_bytes(0x2003, src, 16));  // deliberately misaligned
    check_parity({0xF3, 0x0F, 0x6F, 0x00},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0x2003); },
        false, true);
}

TEST_F(JitParity64Test, MovdquStoreMem) {
    // MOVDQU [RAX], XMM0 — F3 0F 7F 00
    check_parity({0xF3, 0x0F, 0x7F, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x2040);
            cpu.sse.xmm[0].u64_[0] = 0xAABBCCDDEEFF0011ull;
            cpu.sse.xmm[0].u64_[1] = 0x2233445566778899ull;
        },
        false, true);
    // Both interp and jit wrote to shared memory at 0x2040; verify the bytes.
    u8 out[16] = {0};
    ASSERT_TRUE(m_memory->read_bytes(0x2040, out, 16).is_ok());
    EXPECT_EQ(out[0], 0x11); EXPECT_EQ(out[15], 0x22);
}

TEST_F(JitParity64Test, MovdqaLoadReg) {
    // MOVDQA XMM0, XMM1 — 66 0F 6F C1 (register form is native)
    check_parity({0x66, 0x0F, 0x6F, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[1].u64_[0] = 0xCAFEBABEDEADBEEFull;
            cpu.sse.xmm[1].u64_[1] = 0x1020304050607080ull;
        },
        false, true);
}

TEST_F(JitParity64Test, MovdqaStoreReg) {
    // MOVDQA XMM1, XMM0 — 66 0F 7F C1
    check_parity({0x66, 0x0F, 0x7F, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[0].u64_[0] = 0x1111222233334444ull;
            cpu.sse.xmm[0].u64_[1] = 0x5555666677778888ull;
        },
        false, true);
}

TEST_F(JitParity64Test, NativeMovdqaStoreAlignedMemory) {
    // MOVDQA [RAX], XMM8 — aligned memory store with an upper-bank XMM source.
    check_native_parity({0x66, 0x44, 0x0F, 0x7F, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x2040);
            cpu.sse.xmm[8].u64_[0] = 0x0123456789ABCDEFull;
            cpu.sse.xmm[8].u64_[1] = 0xFEDCBA9876543210ull;
        },
        true, true);
    u8 out[16] = {};
    ASSERT_TRUE(m_memory->read_bytes(0x2040, out, sizeof(out)).is_ok());
    EXPECT_EQ(out[0], 0xEF);
    EXPECT_EQ(out[15], 0xFE);
}

TEST_F(JitParity64Test, NativeMovdqaStoreMisalignedRaisesGp) {
    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    const u8 code[] = {0x66, 0x0F, 0x7F, 0x00};  // MOVDQA [RAX],XMM0
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    BasicBlock block = build_block(code, sizeof(code));
    ASSERT_EQ(block.instruction_count, 1u);

    CpuState cpu;
    init_cpu(cpu);
    cpu.gpr.set64(RAX, 0x2041);
    cpu.sse.xmm[0].u64_[0] = 0xAAAAAAAAAAAAAAAAull;
    cpu.sse.xmm[0].u64_[1] = 0xBBBBBBBBBBBBBBBBull;
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    EXPECT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    u8 out[16] = {};
    ASSERT_TRUE(m_memory->read_bytes(0x2041, out, sizeof(out)).is_ok());
    for (u8 byte : out) EXPECT_EQ(byte, 0);
}

TEST_F(JitParity64Test, PshufdReg) {
    // PSHUFD XMM0, XMM1, 0x1B — 66 0F 70 C1 1B  (0b00_01_10_11 → reverse dwords)
    check_parity({0x66, 0x0F, 0x70, 0xC1, 0x1B},
        [](CpuState& cpu) {
            cpu.sse.xmm[1].u32_[0] = 0x11111111;
            cpu.sse.xmm[1].u32_[1] = 0x22222222;
            cpu.sse.xmm[1].u32_[2] = 0x33333333;
            cpu.sse.xmm[1].u32_[3] = 0x44444444;
        },
        false, true);
}

TEST_F(JitParity64Test, PshufdRegBroadcast) {
    // PSHUFD XMM0, XMM1, 0x00 — broadcast dword 0 to all lanes
    check_parity({0x66, 0x0F, 0x70, 0xC1, 0x00},
        [](CpuState& cpu) {
            cpu.sse.xmm[1].u32_[0] = 0xDEADBEEF;
            cpu.sse.xmm[1].u32_[1] = 0x11111111;
            cpu.sse.xmm[1].u32_[2] = 0x22222222;
            cpu.sse.xmm[1].u32_[3] = 0x33333333;
        },
        false, true);
}

TEST_F(JitParity64Test, PshufdRegAlias) {
    // PSHUFD XMM0, XMM0, 0x39 — src==dst aliasing must read all lanes first
    check_parity({0x66, 0x0F, 0x70, 0xC0, 0x39},
        [](CpuState& cpu) {
            cpu.sse.xmm[0].u32_[0] = 0xA0A0A0A0;
            cpu.sse.xmm[0].u32_[1] = 0xB1B1B1B1;
            cpu.sse.xmm[0].u32_[2] = 0xC2C2C2C2;
            cpu.sse.xmm[0].u32_[3] = 0xD3D3D3D3;
        },
        false, true);
}
