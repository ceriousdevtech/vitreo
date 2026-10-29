// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests (32-bit mode): MOVDQU load/store shapes
//
// Fixture and shared helpers: jit_parity_fixture.hpp.

#include "jit_parity_fixture.hpp"

TEST_F(JitParityTest, MovdquMemRoundtrip) {
    alignas(16) u8 src[16] = {
        0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE,
        0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF
    };
    ASSERT_TRUE(m_memory->write_bytes(0x2000, src, sizeof(src)));

    // MOVDQU XMM0, [ESI]
    // MOVDQU [EDI], XMM0
    // MOVDQU XMM1, [EDI]
    check_parity({
            0xF3, 0x0F, 0x6F, 0x06,
            0xF3, 0x0F, 0x7F, 0x07,
            0xF3, 0x0F, 0x6F, 0x0F,
        },
        [](CpuState& cpu) {
            cpu.gpr.set32(ESI, 0x2000);
            cpu.gpr.set32(EDI, 0x2100);
            cpu.sse.xmm[0].clear();
            cpu.sse.xmm[1].clear();
        },
        false, true);
}

TEST_F(JitParityTest, MovdquLoopBodySingleIterShape) {
    alignas(16) u8 src[32];
    for (u8 i = 0; i < 32; ++i) src[i] = static_cast<u8>(0xA0u + i);
    ASSERT_TRUE(m_memory->write_bytes(0x3000, src, sizeof(src)));
    // MOVDQU XMM0, [ESI]
    // MOVDQU XMM1, [ESI+0x10]
    // MOVDQU [EDI], XMM0
    // MOVDQU [EDI+0x10], XMM1
    // LEA ESI, [ESI+0x20]
    // LEA EDI, [EDI+0x20]
    // DEC EDX
    check_parity({
            0x8D, 0x9B, 0x00, 0x00, 0x00, 0x00,
            0xF3, 0x0F, 0x6F, 0x06,
            0xF3, 0x0F, 0x6F, 0x4E, 0x10,
            0xF3, 0x0F, 0x7F, 0x07,
            0xF3, 0x0F, 0x7F, 0x4F, 0x10,
            0x8D, 0x76, 0x20,
            0x8D, 0x7F, 0x20,
            0x4A,
        },
        [](CpuState& cpu) {
            cpu.gpr.set32(EBX, 0x13572468);
            cpu.gpr.set32(ESI, 0x3000);
            cpu.gpr.set32(EDI, 0x3100);
            cpu.gpr.set32(EDX, 2);
            cpu.sse.xmm[0].clear();
            cpu.sse.xmm[1].clear();
        },
        false, true);
}

// Minimal MOVDQU load from [ESI] — one instruction only
TEST_F(JitParityTest, MovdquSingleLoadFromEsi) {
    alignas(16) u8 src[16];
    for (u8 i = 0; i < 16; ++i) src[i] = static_cast<u8>(0xA0u + i);
    ASSERT_TRUE(m_memory->write_bytes(0x3000, src, sizeof(src)));
    check_parity({ 0xF3, 0x0F, 0x6F, 0x06 },  // MOVDQU xmm0, [esi]
        [](CpuState& cpu) {
            cpu.gpr.set32(ESI, 0x3000);
            cpu.sse.xmm[0].clear();
        },
        false, true);
}

// MOVDQU load via absolute displacement
TEST_F(JitParityTest, MovdquSingleLoadFromDisp) {
    alignas(16) u8 src[16];
    for (u8 i = 0; i < 16; ++i) src[i] = static_cast<u8>(0xC0u + i);
    ASSERT_TRUE(m_memory->write_bytes(0x4000, src, sizeof(src)));
    check_parity({ 0xF3, 0x0F, 0x6F, 0x05, 0x00, 0x40, 0x00, 0x00 },  // MOVDQU xmm0, [0x4000]
        [](CpuState& cpu) {
            cpu.sse.xmm[0].clear();
        },
        false, true);
}

// Bisect MovdquLoopBody: just the two MOVDQU loads
TEST_F(JitParityTest, MovdquTwoLoads) {
    alignas(16) u8 src[32];
    for (u8 i = 0; i < 32; ++i) src[i] = static_cast<u8>(0xA0u + i);
    ASSERT_TRUE(m_memory->write_bytes(0x3000, src, sizeof(src)));
    check_parity({
            0xF3, 0x0F, 0x6F, 0x06,         // MOVDQU xmm0, [esi]
            0xF3, 0x0F, 0x6F, 0x4E, 0x10,   // MOVDQU xmm1, [esi+0x10]
        },
        [](CpuState& cpu) {
            cpu.gpr.set32(ESI, 0x3000);
            cpu.sse.xmm[0].clear();
            cpu.sse.xmm[1].clear();
        },
        false, true);
}

// Bisect: preceding LEA + two MOVDQU loads
TEST_F(JitParityTest, MovdquLoadStorePair) {
    alignas(16) u8 src[16];
    for (u8 i = 0; i < 16; ++i) src[i] = static_cast<u8>(0xA0u + i);
    ASSERT_TRUE(m_memory->write_bytes(0x3000, src, sizeof(src)));
    check_parity({
            0xF3, 0x0F, 0x6F, 0x06,    // MOVDQU xmm0, [esi]
            0xF3, 0x0F, 0x7F, 0x07,    // MOVDQU [edi], xmm0
        },
        [](CpuState& cpu) {
            cpu.gpr.set32(ESI, 0x3000);
            cpu.gpr.set32(EDI, 0x3100);
            cpu.sse.xmm[0].clear();
        },
        false, true);
}

TEST_F(JitParityTest, MovdquLoopFullButNoDec) {
    alignas(16) u8 src[32];
    for (u8 i = 0; i < 32; ++i) src[i] = static_cast<u8>(0xA0u + i);
    ASSERT_TRUE(m_memory->write_bytes(0x3000, src, sizeof(src)));
    check_parity({
            0x8D, 0x9B, 0x00, 0x00, 0x00, 0x00,  // LEA EBX, [EBX+0]
            0xF3, 0x0F, 0x6F, 0x06,              // MOVDQU xmm0, [esi]
            0xF3, 0x0F, 0x6F, 0x4E, 0x10,        // MOVDQU xmm1, [esi+0x10]
            0xF3, 0x0F, 0x7F, 0x07,              // MOVDQU [edi], xmm0
            0xF3, 0x0F, 0x7F, 0x4F, 0x10,        // MOVDQU [edi+0x10], xmm1
            0x8D, 0x76, 0x20,                    // LEA ESI, [esi+0x20]
            0x8D, 0x7F, 0x20,                    // LEA EDI, [edi+0x20]
        },
        [](CpuState& cpu) {
            cpu.gpr.set32(EBX, 0x13572468);
            cpu.gpr.set32(ESI, 0x3000);
            cpu.gpr.set32(EDI, 0x3100);
            cpu.sse.xmm[0].clear();
            cpu.sse.xmm[1].clear();
        },
        false, true);
}

TEST_F(JitParityTest, MovdquTwoLoadsOneStore) {
    alignas(16) u8 src[32];
    for (u8 i = 0; i < 32; ++i) src[i] = static_cast<u8>(0xA0u + i);
    ASSERT_TRUE(m_memory->write_bytes(0x3000, src, sizeof(src)));
    check_parity({
            0xF3, 0x0F, 0x6F, 0x06,              // MOVDQU xmm0, [esi]
            0xF3, 0x0F, 0x6F, 0x4E, 0x10,        // MOVDQU xmm1, [esi+0x10]
            0xF3, 0x0F, 0x7F, 0x07,              // MOVDQU [edi], xmm0
        },
        [](CpuState& cpu) {
            cpu.gpr.set32(ESI, 0x3000);
            cpu.gpr.set32(EDI, 0x3100);
            cpu.sse.xmm[0].clear();
            cpu.sse.xmm[1].clear();
        },
        false, true);
}

TEST_F(JitParityTest, MovdquOneLoadTwoStores) {
    alignas(16) u8 src[32];
    for (u8 i = 0; i < 32; ++i) src[i] = static_cast<u8>(0xA0u + i);
    ASSERT_TRUE(m_memory->write_bytes(0x3000, src, sizeof(src)));
    check_parity({
            0xF3, 0x0F, 0x6F, 0x06,              // MOVDQU xmm0, [esi]
            0xF3, 0x0F, 0x7F, 0x07,              // MOVDQU [edi], xmm0
            0xF3, 0x0F, 0x7F, 0x4F, 0x10,        // MOVDQU [edi+0x10], xmm0
        },
        [](CpuState& cpu) {
            cpu.gpr.set32(ESI, 0x3000);
            cpu.gpr.set32(EDI, 0x3100);
            cpu.sse.xmm[0].clear();
        },
        false, true);
}

TEST_F(JitParityTest, MovdquFourLoads) {
    alignas(16) u8 src[64];
    for (u8 i = 0; i < 64; ++i) src[i] = static_cast<u8>(0x40u + i);
    ASSERT_TRUE(m_memory->write_bytes(0x3000, src, sizeof(src)));
    check_parity({
            0xF3, 0x0F, 0x6F, 0x06,              // MOVDQU xmm0, [esi]
            0xF3, 0x0F, 0x6F, 0x4E, 0x10,        // MOVDQU xmm1, [esi+0x10]
            0xF3, 0x0F, 0x6F, 0x56, 0x20,        // MOVDQU xmm2, [esi+0x20]
            0xF3, 0x0F, 0x6F, 0x5E, 0x30,        // MOVDQU xmm3, [esi+0x30]
        },
        [](CpuState& cpu) {
            cpu.gpr.set32(ESI, 0x3000);
            for (int i = 0; i < 4; ++i) cpu.sse.xmm[i].clear();
        },
        false, true);
}

TEST_F(JitParityTest, MovdquTwoStoresOnly) {
    alignas(16) u8 src[32];
    for (u8 i = 0; i < 32; ++i) src[i] = static_cast<u8>(0xA0u + i);
    ASSERT_TRUE(m_memory->write_bytes(0x3000, src, sizeof(src)));
    check_parity({
            0xF3, 0x0F, 0x7F, 0x07,              // MOVDQU [edi], xmm0
            0xF3, 0x0F, 0x7F, 0x4F, 0x10,        // MOVDQU [edi+0x10], xmm1
        },
        [](CpuState& cpu) {
            cpu.gpr.set32(EDI, 0x3100);
            for (int i = 0; i < 16; ++i) {
                cpu.sse.xmm[0].u8_[i] = static_cast<u8>(0xE0 + i);
                cpu.sse.xmm[1].u8_[i] = static_cast<u8>(0xF0 + i);
            }
        },
        false, true);
}

TEST_F(JitParityTest, MovdquFourStores) {
    check_parity({
            0xF3, 0x0F, 0x7F, 0x07,              // MOVDQU [edi], xmm0
            0xF3, 0x0F, 0x7F, 0x4F, 0x10,        // MOVDQU [edi+0x10], xmm1
            0xF3, 0x0F, 0x7F, 0x57, 0x20,        // MOVDQU [edi+0x20], xmm2
            0xF3, 0x0F, 0x7F, 0x5F, 0x30,        // MOVDQU [edi+0x30], xmm3
        },
        [](CpuState& cpu) {
            cpu.gpr.set32(EDI, 0x3100);
            for (int i = 0; i < 16; ++i) {
                cpu.sse.xmm[0].u8_[i] = static_cast<u8>(0xE0 + i);
                cpu.sse.xmm[1].u8_[i] = static_cast<u8>(0xF0 + i);
                cpu.sse.xmm[2].u8_[i] = static_cast<u8>(0x10 + i);
                cpu.sse.xmm[3].u8_[i] = static_cast<u8>(0x20 + i);
            }
        },
        false, true);
}

TEST_F(JitParityTest, MovdquTwoLoadsTwoStoresSameXmm) {
    // Load 2 xmms, store twice from xmm0 (not using xmm1 for store)
    alignas(16) u8 src[32];
    for (u8 i = 0; i < 32; ++i) src[i] = static_cast<u8>(0xA0u + i);
    ASSERT_TRUE(m_memory->write_bytes(0x3000, src, sizeof(src)));
    check_parity({
            0xF3, 0x0F, 0x6F, 0x06,              // MOVDQU xmm0, [esi]
            0xF3, 0x0F, 0x6F, 0x4E, 0x10,        // MOVDQU xmm1, [esi+0x10]
            0xF3, 0x0F, 0x7F, 0x07,              // MOVDQU [edi], xmm0
            0xF3, 0x0F, 0x7F, 0x47, 0x10,        // MOVDQU [edi+0x10], xmm0
        },
        [](CpuState& cpu) {
            cpu.gpr.set32(ESI, 0x3000);
            cpu.gpr.set32(EDI, 0x3100);
            cpu.sse.xmm[0].clear();
            cpu.sse.xmm[1].clear();
        },
        false, true);
}

TEST_F(JitParityTest, MovdquStoreLoadStoreLoad) {
    // Interleaved S L S L to verify order
    alignas(16) u8 src[32];
    for (u8 i = 0; i < 32; ++i) src[i] = static_cast<u8>(0xA0u + i);
    ASSERT_TRUE(m_memory->write_bytes(0x3000, src, sizeof(src)));
    check_parity({
            0xF3, 0x0F, 0x7F, 0x17,              // MOVDQU [edi], xmm2
            0xF3, 0x0F, 0x6F, 0x06,              // MOVDQU xmm0, [esi]
            0xF3, 0x0F, 0x7F, 0x4F, 0x20,        // MOVDQU [edi+0x20], xmm1
            0xF3, 0x0F, 0x6F, 0x4E, 0x10,        // MOVDQU xmm1, [esi+0x10]
        },
        [](CpuState& cpu) {
            cpu.gpr.set32(ESI, 0x3000);
            cpu.gpr.set32(EDI, 0x3100);
            cpu.sse.xmm[0].clear();
            for (int i = 0; i < 16; ++i) cpu.sse.xmm[1].u8_[i] = static_cast<u8>(0x50+i);
            for (int i = 0; i < 16; ++i) cpu.sse.xmm[2].u8_[i] = static_cast<u8>(0x60+i);
        },
        false, true);
}

TEST_F(JitParityTest, MovdquOneLoadTwoStoresDiffXmm) {
    alignas(16) u8 src[16];
    for (u8 i = 0; i < 16; ++i) src[i] = static_cast<u8>(0xA0u + i);
    ASSERT_TRUE(m_memory->write_bytes(0x3000, src, sizeof(src)));
    check_parity({
            0xF3, 0x0F, 0x6F, 0x06,              // MOVDQU xmm0, [esi]
            0xF3, 0x0F, 0x7F, 0x07,              // MOVDQU [edi], xmm0
            0xF3, 0x0F, 0x7F, 0x4F, 0x10,        // MOVDQU [edi+0x10], xmm1
        },
        [](CpuState& cpu) {
            cpu.gpr.set32(ESI, 0x3000);
            cpu.gpr.set32(EDI, 0x3100);
            cpu.sse.xmm[0].clear();
            for (int i = 0; i < 16; ++i) cpu.sse.xmm[1].u8_[i] = static_cast<u8>(0x50 + i);
        },
        false, true);
}

TEST_F(JitParityTest, MovdquTwoStoresDiffXmm) {
    check_parity({
            0xF3, 0x0F, 0x7F, 0x07,              // MOVDQU [edi], xmm0
            0xF3, 0x0F, 0x7F, 0x4F, 0x10,        // MOVDQU [edi+0x10], xmm1
        },
        [](CpuState& cpu) {
            cpu.gpr.set32(EDI, 0x3100);
            for (int i = 0; i < 16; ++i) cpu.sse.xmm[0].u8_[i] = static_cast<u8>(0xE0+i);
            for (int i = 0; i < 16; ++i) cpu.sse.xmm[1].u8_[i] = static_cast<u8>(0x50+i);
        },
        false, true);
}

TEST_F(JitParityTest, MovdquTwoLoadsTwoStores) {
    alignas(16) u8 src[32];
    for (u8 i = 0; i < 32; ++i) src[i] = static_cast<u8>(0xA0u + i);
    ASSERT_TRUE(m_memory->write_bytes(0x3000, src, sizeof(src)));
    check_parity({
            0xF3, 0x0F, 0x6F, 0x06,              // MOVDQU xmm0, [esi]
            0xF3, 0x0F, 0x6F, 0x4E, 0x10,        // MOVDQU xmm1, [esi+0x10]
            0xF3, 0x0F, 0x7F, 0x07,              // MOVDQU [edi], xmm0
            0xF3, 0x0F, 0x7F, 0x4F, 0x10,        // MOVDQU [edi+0x10], xmm1
        },
        [](CpuState& cpu) {
            cpu.gpr.set32(ESI, 0x3000);
            cpu.gpr.set32(EDI, 0x3100);
            cpu.sse.xmm[0].clear();
            cpu.sse.xmm[1].clear();
        },
        false, true);
}

TEST_F(JitParityTest, MovdquLoadsStoresAndOneLea) {
    alignas(16) u8 src[32];
    for (u8 i = 0; i < 32; ++i) src[i] = static_cast<u8>(0xA0u + i);
    ASSERT_TRUE(m_memory->write_bytes(0x3000, src, sizeof(src)));
    check_parity({
            0xF3, 0x0F, 0x6F, 0x06,              // MOVDQU xmm0, [esi]
            0xF3, 0x0F, 0x6F, 0x4E, 0x10,        // MOVDQU xmm1, [esi+0x10]
            0xF3, 0x0F, 0x7F, 0x07,              // MOVDQU [edi], xmm0
            0xF3, 0x0F, 0x7F, 0x4F, 0x10,        // MOVDQU [edi+0x10], xmm1
            0x8D, 0x76, 0x20,                    // LEA ESI, [esi+0x20]
        },
        [](CpuState& cpu) {
            cpu.gpr.set32(ESI, 0x3000);
            cpu.gpr.set32(EDI, 0x3100);
            cpu.sse.xmm[0].clear();
            cpu.sse.xmm[1].clear();
        },
        false, true);
}

TEST_F(JitParityTest, MovdquJustDec) {
    // Two loads + DEC EDX.
    alignas(16) u8 src[32];
    for (u8 i = 0; i < 32; ++i) src[i] = static_cast<u8>(0xA0u + i);
    ASSERT_TRUE(m_memory->write_bytes(0x3000, src, sizeof(src)));
    check_parity({
            0xF3, 0x0F, 0x6F, 0x06,         // MOVDQU xmm0, [esi]
            0xF3, 0x0F, 0x6F, 0x4E, 0x10,   // MOVDQU xmm1, [esi+0x10]
            0x4A,                           // DEC EDX
        },
        [](CpuState& cpu) {
            cpu.gpr.set32(ESI, 0x3000);
            cpu.gpr.set32(EDX, 2);
            cpu.sse.xmm[0].clear();
            cpu.sse.xmm[1].clear();
        },
        false, true);
}

// Bisect: preceding LEA + two MOVDQU loads
TEST_F(JitParityTest, MovdquLeaThenTwoLoads) {
    alignas(16) u8 src[32];
    for (u8 i = 0; i < 32; ++i) src[i] = static_cast<u8>(0xA0u + i);
    ASSERT_TRUE(m_memory->write_bytes(0x3000, src, sizeof(src)));
    check_parity({
            0x8D, 0x9B, 0x00, 0x00, 0x00, 0x00,  // LEA EBX, [EBX+0]
            0xF3, 0x0F, 0x6F, 0x06,              // MOVDQU xmm0, [esi]
            0xF3, 0x0F, 0x6F, 0x4E, 0x10,        // MOVDQU xmm1, [esi+0x10]
        },
        [](CpuState& cpu) {
            cpu.gpr.set32(EBX, 0x13572468);
            cpu.gpr.set32(ESI, 0x3000);
            cpu.sse.xmm[0].clear();
            cpu.sse.xmm[1].clear();
        },
        false, true);
}
