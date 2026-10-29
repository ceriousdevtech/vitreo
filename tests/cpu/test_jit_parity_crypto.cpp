// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests (32-bit mode): SHA-NI, AES-NI and PCLMULQDQ
//
// Fixture and shared helpers: jit_parity_fixture.hpp.

#include "jit_parity_fixture.hpp"

// ============================================================================
// SHA-NI JIT vs Interpreter parity
// ============================================================================

TEST_F(JitParityTest, Sha1msg2PreservesIntelLaneOrder) {
    // SHA1MSG2 XMM0, XMM1 — the asymmetric lanes catch the ARM helper
    // accidentally expanding W16..W19 from least- to most-significant lanes.
    const u8 code[] = {0x0F, 0x38, 0xCA, 0xC1};
    check_parity(code, sizeof(code),
        [](CpuState& cpu) {
            cpu.sse.xmm[0].u32_[0] = 0x01234567u;
            cpu.sse.xmm[0].u32_[1] = 0x89ABCDEFu;
            cpu.sse.xmm[0].u32_[2] = 0xFEDCBA98u;
            cpu.sse.xmm[0].u32_[3] = 0x76543210u;
            cpu.sse.xmm[1].u32_[0] = 0x0F1E2D3Cu;
            cpu.sse.xmm[1].u32_[1] = 0x4B5A6978u;
            cpu.sse.xmm[1].u32_[2] = 0x8796A5B4u;
            cpu.sse.xmm[1].u32_[3] = 0xC3D2E1F0u;
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

// ============================================================================
// AES-NI JIT vs Interpreter parity
// (The interpreter path is FIPS-197 verified in test_crypto_ops.cpp.
//  This test catches any divergence between JIT and interpreter for AES.)
// ============================================================================

TEST_F(JitParityTest, AesencRegRegFips197Round1) {
    // AESENC xmm0, xmm1 — 66 0F 38 DC C1
    u8 code[] = { 0x66, 0x0F, 0x38, 0xDC, 0xC1 };
    check_parity(code, sizeof(code),
        [](CpuState& cpu) {
            const u8 state_bytes[16] = {
                0x19,0x3d,0xe3,0xbe, 0xa0,0xf4,0xe2,0x2b,
                0x9a,0xc6,0x8d,0x2a, 0xe9,0xf8,0x48,0x08
            };
            const u8 key_bytes[16] = {
                0xa0,0xfa,0xfe,0x17, 0x88,0x54,0x2c,0xb1,
                0x23,0xa3,0x39,0x39, 0x2a,0x6c,0x76,0x05
            };
            std::memcpy(&cpu.sse.xmm[0], state_bytes, 16);
            std::memcpy(&cpu.sse.xmm[1], key_bytes,   16);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParityTest, AesencMemOperand) {
    // AESENC xmm0, [eax] — 66 0F 38 DC 00 (ModRM 00 = [EAX])
    u8 code[] = { 0x66, 0x0F, 0x38, 0xDC, 0x00 };
    const u8 key_bytes[16] = {
        0xa0,0xfa,0xfe,0x17, 0x88,0x54,0x2c,0xb1,
        0x23,0xa3,0x39,0x39, 0x2a,0x6c,0x76,0x05
    };
    m_memory->write_bytes(0x4000, key_bytes, 16);
    check_parity(code, sizeof(code),
        [](CpuState& cpu) {
            const u8 state_bytes[16] = {
                0x19,0x3d,0xe3,0xbe, 0xa0,0xf4,0xe2,0x2b,
                0x9a,0xc6,0x8d,0x2a, 0xe9,0xf8,0x48,0x08
            };
            std::memcpy(&cpu.sse.xmm[0], state_bytes, 16);
            cpu.gpr.set32(EAX, 0x4000);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParityTest, AesenclastRegReg) {
    // AESENCLAST xmm0, xmm1 — 66 0F 38 DD C1
    u8 code[] = { 0x66, 0x0F, 0x38, 0xDD, 0xC1 };
    check_parity(code, sizeof(code),
        [](CpuState& cpu) {
            const u8 state_bytes[16] = {
                0xe1,0xe8,0x35,0x97, 0xfb,0xc8,0x6c,0x4f,
                0x96,0xae,0xd2,0xfb, 0x7c,0x9b,0xba,0x53
            };
            const u8 key_bytes[16] = {
                0xd0,0x14,0xf9,0xa8, 0xc9,0xee,0x25,0x89,
                0xe1,0x3f,0x0c,0xc8, 0xb6,0x63,0x0c,0xa6
            };
            std::memcpy(&cpu.sse.xmm[0], state_bytes, 16);
            std::memcpy(&cpu.sse.xmm[1], key_bytes,   16);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

// Multi-op stream mimicking OpenSSL's AES-GCM stitched loop:
// AESENC, AESENC, AESENCLAST — catches any state-carry bug between
// consecutive AES ops in the same JIT block.
TEST_F(JitParityTest, AesencStreamThreeOps) {
    u8 code[] = {
        0x66, 0x0F, 0x38, 0xDC, 0xC1,  // AESENC  xmm0, xmm1
        0x66, 0x0F, 0x38, 0xDC, 0xC2,  // AESENC  xmm0, xmm2
        0x66, 0x0F, 0x38, 0xDD, 0xC3,  // AESENCLAST xmm0, xmm3
    };
    check_parity(code, sizeof(code),
        [](CpuState& cpu) {
            // deterministic but non-uniform initial state & round keys
            for (int b = 0; b < 16; ++b) cpu.sse.xmm[0].u8_[b] = static_cast<u8>(0x10 + b);
            for (int b = 0; b < 16; ++b) cpu.sse.xmm[1].u8_[b] = static_cast<u8>(0xA0 + b);
            for (int b = 0; b < 16; ++b) cpu.sse.xmm[2].u8_[b] = static_cast<u8>(0xB0 + b);
            for (int b = 0; b < 16; ++b) cpu.sse.xmm[3].u8_[b] = static_cast<u8>(0xC0 + b);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParityTest, Pclmulqdq) {
    // PCLMULQDQ xmm0, xmm1, 0x11 — 66 0F 3A 44 C1 11
    // imm8=0x11: select high qword of dst (xmm0) and high qword of src (xmm1)
    u8 code[] = { 0x66, 0x0F, 0x3A, 0x44, 0xC1, 0x11 };
    check_parity(code, sizeof(code),
        [](CpuState& cpu) {
            cpu.sse.xmm[0].u64_[0] = 0xDEADBEEFCAFEBABEull;
            cpu.sse.xmm[0].u64_[1] = 0x0123456789ABCDEFull;
            cpu.sse.xmm[1].u64_[0] = 0x1111111122222222ull;
            cpu.sse.xmm[1].u64_[1] = 0xFEEDFACEDECAFBADull;
        },
        false, true);
}

TEST_F(JitParityTest, PclmulqdqAllImmVariants) {
    // Verify all 4 imm8 variants (0x00, 0x01, 0x10, 0x11).
    for (u8 imm : {0x00, 0x01, 0x10, 0x11}) {
        u8 code[] = { 0x66, 0x0F, 0x3A, 0x44, 0xC1, imm };
        check_parity(code, sizeof(code),
            [](CpuState& cpu) {
                cpu.sse.xmm[0].u64_[0] = 0x0123456789ABCDEFull;
                cpu.sse.xmm[0].u64_[1] = 0xFEDCBA9876543210ull;
                cpu.sse.xmm[1].u64_[0] = 0xAAAAAAAAAAAAAAAAull;
                cpu.sse.xmm[1].u64_[1] = 0x5555555555555555ull;
            },
            false, true);
    }
}

TEST_F(JitParityTest, Aeskeygenassist) {
    // AESKEYGENASSIST xmm0, xmm1, 0x01 — 66 0F 3A DF C1 01
    u8 code[] = { 0x66, 0x0F, 0x3A, 0xDF, 0xC1, 0x01 };
    check_parity(code, sizeof(code),
        [](CpuState& cpu) {
            const u8 key_bytes[16] = {
                0x2b,0x7e,0x15,0x16, 0x28,0xae,0xd2,0xa6,
                0xab,0xf7,0x15,0x88, 0x09,0xcf,0x4f,0x3c
            };
            std::memcpy(&cpu.sse.xmm[1], key_bytes, 16);
        },
        false, true);
}

// AES-GCM stitched loop proxy: AES round interleaved with PCLMULQDQ+PSHUFB,
// similar to OpenSSL's aesni_gcm_encrypt inner loop.
TEST_F(JitParityTest, AesGcmStitchedProxy) {
    u8 code[] = {
        0x66, 0x0F, 0x38, 0xDC, 0xC1,        // AESENC xmm0, xmm1
        0x66, 0x0F, 0x3A, 0x44, 0xDA, 0x00,  // PCLMULQDQ xmm3, xmm2, 0
        0x66, 0x0F, 0x38, 0x00, 0xE4,        // PSHUFB xmm4, xmm4
        0x66, 0x0F, 0x38, 0xDC, 0xC1,        // AESENC xmm0, xmm1
        0x66, 0x0F, 0x3A, 0x44, 0xDA, 0x11,  // PCLMULQDQ xmm3, xmm2, 0x11
        0x66, 0x0F, 0x38, 0xDD, 0xC1,        // AESENCLAST xmm0, xmm1
    };
    auto init = [](CpuState& cpu) {
        for (int r = 0; r < 8; ++r)
            for (int b = 0; b < 16; ++b)
                cpu.sse.xmm[r].u8_[b] = static_cast<u8>((r * 0x11) + b);
    };
    check_parity(code, sizeof(code), init, false, true);
}

// Bisection: AES + PCLMULQDQ pair
TEST_F(JitParityTest, AesencThenPclmul) {
    u8 code[] = {
        0x66, 0x0F, 0x38, 0xDC, 0xC1,        // AESENC xmm0, xmm1
        0x66, 0x0F, 0x3A, 0x44, 0xDA, 0x00,  // PCLMULQDQ xmm3, xmm2, 0
    };
    auto init = [](CpuState& cpu) {
        for (int r = 0; r < 8; ++r)
            for (int b = 0; b < 16; ++b)
                cpu.sse.xmm[r].u8_[b] = static_cast<u8>((r * 0x11) + b);
    };
    check_parity(code, sizeof(code), init, false, true);
}

// Bisection: PCLMULQDQ + AES pair
TEST_F(JitParityTest, PclmulThenAesenc) {
    u8 code[] = {
        0x66, 0x0F, 0x3A, 0x44, 0xDA, 0x00,  // PCLMULQDQ xmm3, xmm2, 0
        0x66, 0x0F, 0x38, 0xDC, 0xC1,        // AESENC xmm0, xmm1
    };
    auto init = [](CpuState& cpu) {
        for (int r = 0; r < 8; ++r)
            for (int b = 0; b < 16; ++b)
                cpu.sse.xmm[r].u8_[b] = static_cast<u8>((r * 0x11) + b);
    };
    check_parity(code, sizeof(code), init, false, true);
}

// Bisection: AES + PSHUFB pair
TEST_F(JitParityTest, AesencThenPshufb) {
    u8 code[] = {
        0x66, 0x0F, 0x38, 0xDC, 0xC1,        // AESENC xmm0, xmm1
        0x66, 0x0F, 0x38, 0x00, 0xE4,        // PSHUFB xmm4, xmm4
    };
    auto init = [](CpuState& cpu) {
        for (int r = 0; r < 8; ++r)
            for (int b = 0; b < 16; ++b)
                cpu.sse.xmm[r].u8_[b] = static_cast<u8>((r * 0x11) + b);
    };
    check_parity(code, sizeof(code), init, false, true);
}

// Bisection: PCLMULQDQ + PSHUFB pair
TEST_F(JitParityTest, PclmulThenPshufb) {
    u8 code[] = {
        0x66, 0x0F, 0x3A, 0x44, 0xDA, 0x00,  // PCLMULQDQ xmm3, xmm2, 0
        0x66, 0x0F, 0x38, 0x00, 0xE4,        // PSHUFB xmm4, xmm4
    };
    auto init = [](CpuState& cpu) {
        for (int r = 0; r < 8; ++r)
            for (int b = 0; b < 16; ++b)
                cpu.sse.xmm[r].u8_[b] = static_cast<u8>((r * 0x11) + b);
    };
    check_parity(code, sizeof(code), init, false, true);
}
