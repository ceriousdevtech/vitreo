// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests (32-bit mode): SSE
//
// Fixture and shared helpers: jit_parity_fixture.hpp.

#include "jit_parity_fixture.hpp"

// ============================================================================
// SSE parity (via JIT's sse_op helper)
// ============================================================================

TEST_F(JitParityTest, MovdqaRegReg) {
    // MOVDQA XMM0, XMM1 — 66 0F 6F C1
    check_parity({0x66, 0x0F, 0x6F, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[1].u64_[0] = 0x0123456789ABCDEFull;
            cpu.sse.xmm[1].u64_[1] = 0xFEDCBA9876543210ull;
        },
        false, true);
}

TEST_F(JitParityTest, MovdquRegReg) {
    // MOVDQU XMM0, XMM1 — F3 0F 6F C1
    check_parity({0xF3, 0x0F, 0x6F, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[0].u64_[0] = 0;
            cpu.sse.xmm[0].u64_[1] = 0;
            cpu.sse.xmm[1].u64_[0] = 0x1122334455667788ull;
            cpu.sse.xmm[1].u64_[1] = 0x99AABBCCDDEEFF00ull;
        },
        false, true);
}

TEST_F(JitParityTest, MovdqaStoreEncodingRegReg) {
    // MOVDQA XMM1, XMM0 through the 66 0F 7F /r encoding.
    check_parity({0x66, 0x0F, 0x7F, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[0].u64_[0] = 0x89ABCDEF01234567ull;
            cpu.sse.xmm[0].u64_[1] = 0x76543210FEDCBA98ull;
            cpu.sse.xmm[1].u64_[0] = 0;
            cpu.sse.xmm[1].u64_[1] = 0;
        },
        false, true);
}

TEST_F(JitParityTest, MovlpdStoreLowLane) {
    // MOVLPD [EBX+4], XMM0 - 66 0F 13 43 04
    check_parity({0x66, 0x0F, 0x13, 0x43, 0x04},
        [](CpuState& cpu) {
            cpu.gpr.set32(EBX, 0x3000);
            cpu.sse.xmm[0].u64_[0] = 0x0123456789ABCDEFull;
            cpu.sse.xmm[0].u64_[1] = 0xFEDCBA9876543210ull;
        });
}

TEST_F(JitParityTest, MovlpsStoreLowLane) {
    // MOVLPS [EBX+8], XMM1 - 0F 13 4B 08
    check_parity({0x0F, 0x13, 0x4B, 0x08},
        [](CpuState& cpu) {
            cpu.gpr.set32(EBX, 0x3000);
            cpu.sse.xmm[1].u64_[0] = 0x8899AABBCCDDEEFFull;
            cpu.sse.xmm[1].u64_[1] = 0x0011223344556677ull;
        });
}

TEST_F(JitParityTest, MovhpsLoadHighLane) {
    const u64 src = 0x1122334455667788ull;
    ASSERT_TRUE(m_memory->write_u64(0x3000, src));
    // MOVHPS XMM0, [EBX] — 0F 16 03
    check_parity({0x0F, 0x16, 0x03},
        [](CpuState& cpu) {
            cpu.gpr.set32(EBX, 0x3000);
            cpu.sse.xmm[0].u64_[0] = 0xAAAAAAAAAAAAAAAAULL;
            cpu.sse.xmm[0].u64_[1] = 0xBBBBBBBBBBBBBBBBULL;
        },
        true, true);
}

TEST_F(JitParityTest, PinsrdMemorySource) {
    ASSERT_TRUE(m_memory->write_u32(0x3000, 0xAABBCCDDu));
    // PINSRD XMM0, [EBX], 1 — 66 0F 3A 22 03 01
    check_parity({0x66, 0x0F, 0x3A, 0x22, 0x03, 0x01},
        [](CpuState& cpu) {
            cpu.gpr.set32(EBX, 0x3000);
            cpu.sse.xmm[0].u32_[0] = 1;
            cpu.sse.xmm[0].u32_[1] = 2;
            cpu.sse.xmm[0].u32_[2] = 3;
            cpu.sse.xmm[0].u32_[3] = 4;
        },
        true, true);
}

TEST_F(JitParityTest, InsertpsMemorySourceIgnoresCountS) {
    // Intel SDM INSERTPS: memory source forces COUNT_S = 0.
    ASSERT_TRUE(m_memory->write_u32(0x3000, 0x3F800000u));  // 1.0f
    // INSERTPS XMM0, [EBX], 0x50 — dest lane 1, COUNT_S would be 1 if reg
    check_parity({0x66, 0x0F, 0x3A, 0x21, 0x03, 0x50},
        [](CpuState& cpu) {
            cpu.gpr.set32(EBX, 0x3000);
            cpu.sse.xmm[0].u32_[0] = 0x11111111u;
            cpu.sse.xmm[0].u32_[1] = 0x22222222u;
            cpu.sse.xmm[0].u32_[2] = 0x33333333u;
            cpu.sse.xmm[0].u32_[3] = 0x44444444u;
        },
        true, true);
}

TEST_F(JitParityTest, BtsMemImm8SetsBitAndCf) {
    // Intel SDM Vol. 2 BTS r/m32, imm8: CF := old bit; then set the bit.
    ASSERT_TRUE(m_memory->write_u32(0x3000, 0x00000001u));
    // BTS dword ptr [EBX], 3 — 0F BA /5 ib
    check_parity({0x0F, 0xBA, 0x2B, 0x03},
        [](CpuState& cpu) {
            cpu.gpr.set32(EBX, 0x3000);
        },
        true, true);
}

TEST_F(JitParityTest, CmpssMemoryEqPreservesUpper) {
    ASSERT_TRUE(m_memory->write_u32(0x3000, 0x3F800000u));  // 1.0f
    // CMPSS XMM0, [EBX], 0 — EQ
    check_parity({0xF3, 0x0F, 0xC2, 0x03, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set32(EBX, 0x3000);
            cpu.sse.xmm[0].u32_[0] = 0x3F800000u;
            cpu.sse.xmm[0].u32_[1] = 0x40000000u;
            cpu.sse.xmm[0].u32_[2] = 0x40400000u;
            cpu.sse.xmm[0].u32_[3] = 0x40800000u;
        },
        true, true);
}

TEST_F(JitParityTest, Cvtsi2ssEcxPreservesUpperXmm) {
    // CVTSI2SS XMM0, ECX — F3 0F 2A C1
    check_parity({0xF3, 0x0F, 0x2A, 0xC1},
        [](CpuState& cpu) {
            cpu.gpr.set32(ECX, 42);
            cpu.sse.xmm[0].u32_[0] = 0x3F800000u;
            cpu.sse.xmm[0].u32_[1] = 0x40000000u;
            cpu.sse.xmm[0].u32_[2] = 0x40400000u;
            cpu.sse.xmm[0].u32_[3] = 0x40800000u;
        },
        true, true);
}

TEST_F(JitParityTest, PadddRegReg) {
    // PADDD XMM0, XMM1 — 66 0F FE C1
    check_parity({0x66, 0x0F, 0xFE, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[0].u32_[0] = 100;
            cpu.sse.xmm[0].u32_[1] = 200;
            cpu.sse.xmm[0].u32_[2] = 300;
            cpu.sse.xmm[0].u32_[3] = 400;
            cpu.sse.xmm[1].u32_[0] = 10;
            cpu.sse.xmm[1].u32_[1] = 20;
            cpu.sse.xmm[1].u32_[2] = 30;
            cpu.sse.xmm[1].u32_[3] = 40;
        },
        false, true);
}

TEST_F(JitParityTest, PxorRegReg) {
    // PXOR XMM0, XMM1 — 66 0F EF C1
    check_parity({0x66, 0x0F, 0xEF, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[0].u64_[0] = 0xAAAAAAAAAAAAAAAAull;
            cpu.sse.xmm[0].u64_[1] = 0x5555555555555555ull;
            cpu.sse.xmm[1].u64_[0] = 0xFFFFFFFFFFFFFFFFull;
            cpu.sse.xmm[1].u64_[1] = 0xFFFFFFFFFFFFFFFFull;
        },
        false, true);
}

TEST_F(JitParityTest, PmaddwdRegReg) {
    // 66 0F F5 C1 — PMADDWD xmm0, xmm1
    check_parity({0x66, 0x0F, 0xF5, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[0].i16_[0] = 3;
            cpu.sse.xmm[0].i16_[1] = 5;
            cpu.sse.xmm[0].i16_[2] = 10;
            cpu.sse.xmm[0].i16_[3] = 7;
            cpu.sse.xmm[0].i16_[4] = -1;
            cpu.sse.xmm[0].i16_[5] = 2;
            cpu.sse.xmm[0].i16_[6] = -32768;
            cpu.sse.xmm[0].i16_[7] = -32768;
            cpu.sse.xmm[1].i16_[0] = 4;
            cpu.sse.xmm[1].i16_[1] = 6;
            cpu.sse.xmm[1].i16_[2] = -2;
            cpu.sse.xmm[1].i16_[3] = 3;
            cpu.sse.xmm[1].i16_[4] = 8;
            cpu.sse.xmm[1].i16_[5] = 9;
            cpu.sse.xmm[1].i16_[6] = -32768;
            cpu.sse.xmm[1].i16_[7] = -32768;
        },
        false, true);
}

TEST_F(JitParityTest, MovlhpsRegReg) {
    // 0F 16 C1 — MOVLHPS xmm0, xmm1
    check_parity({0x0F, 0x16, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[0].u64_[0] = 0x1111111111111111ull;
            cpu.sse.xmm[0].u64_[1] = 0x2222222222222222ull;
            cpu.sse.xmm[1].u64_[0] = 0xAAAAAAAAAAAAAAAAULL;
            cpu.sse.xmm[1].u64_[1] = 0xBBBBBBBBBBBBBBBBULL;
        },
        false, true);
}

TEST_F(JitParityTest, NativeXorpsMemorySource) {
    alignas(16) const u8 source[16] = {
        0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
        0xF0, 0xDE, 0xBC, 0x9A, 0x78, 0x56, 0x34, 0x12,
    };
    // 16-byte ALIGNED, and that is the contract rather than a convenience:
    // legacy SSE raises #GP on a misaligned 128-bit memory operand for every
    // one of these forms, not only the MOVAPS/MOVDQA copies. Measured on
    // silicon -- tools/oracle_probe/sse_align_probe.c and its recorded output.
    // This test used [ESI+8] and called it "unaligned"; it was asserting
    // behaviour no real CPU has.
    ASSERT_TRUE(m_memory->write_bytes(0x3010, source, sizeof(source)));
    // XORPS XMM0,[ESI+16] — 0F 57 46 10.
    check_parity({0x0F, 0x57, 0x46, 0x10},
        [](CpuState& cpu) {
            cpu.gpr.set32(ESI, 0x3000);
            cpu.sse.xmm[0].u64_[0] = 0xFFFF0000FFFF0000ull;
            cpu.sse.xmm[0].u64_[1] = 0xAAAAAAAA55555555ull;
            cpu.flags.rflags = 0x8D5u;
        },
        true, true);
    // The misaligned spelling must RAISE on both tiers. `check_parity`
    // asserts the interpreter completed, so it cannot express that; the
    // raise-versus-raise comparison lives in
    // IrEmit64NarrowAlu.VectorBitwiseMemoryOperandsRequireAlignment, which
    // drives all sixteen alignments and counts how many faulted.
}

TEST_F(JitParityTest, NativePmovmskbBitOrderAndFlags) {
    // PMOVMSKB EAX,XMM1 — 66 0F D7 C1.
    check_parity({0x66, 0x0F, 0xD7, 0xC1},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xFFFFFFFFu);
            for (u8 i = 0; i < 16; ++i) {
                cpu.sse.xmm[1].u8_[i] =
                    static_cast<u8>((i == 0 || i == 3 || i == 8 || i == 15)
                                        ? (0x80u | i)
                                        : i);
            }
            cpu.flags.rflags = 0x8D5u;
        },
        true, true);
}

TEST_F(JitParityTest, PandRegReg) {
    // PAND XMM0, XMM1 — 66 0F DB C1
    check_parity({0x66, 0x0F, 0xDB, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[0].u64_[0] = 0xFF00FF00FF00FF00ull;
            cpu.sse.xmm[0].u64_[1] = 0x00FF00FF00FF00FFull;
            cpu.sse.xmm[1].u64_[0] = 0x0F0F0F0F0F0F0F0Full;
            cpu.sse.xmm[1].u64_[1] = 0xF0F0F0F0F0F0F0F0ull;
        },
        false, true);
}

TEST_F(JitParityTest, PorRegReg) {
    // POR XMM0, XMM1 — 66 0F EB C1
    check_parity({0x66, 0x0F, 0xEB, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[0].u64_[0] = 0xFF00FF00FF00FF00ull;
            cpu.sse.xmm[1].u64_[0] = 0x00FF00FF00FF00FFull;
        },
        false, true);
}

TEST_F(JitParityTest, PcmpeqdRegReg) {
    // PCMPEQD XMM0, XMM1 — 66 0F 76 C1
    check_parity({0x66, 0x0F, 0x76, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[0].u32_[0] = 42;
            cpu.sse.xmm[0].u32_[1] = 100;
            cpu.sse.xmm[0].u32_[2] = 42;
            cpu.sse.xmm[0].u32_[3] = 200;
            cpu.sse.xmm[1].u32_[0] = 42;
            cpu.sse.xmm[1].u32_[1] = 999;
            cpu.sse.xmm[1].u32_[2] = 42;
            cpu.sse.xmm[1].u32_[3] = 200;
        },
        false, true);
}

TEST_F(JitParityTest, PshufdImm) {
    // PSHUFD XMM0, XMM1, 0x1B — 66 0F 70 C1 1B
    // 0x1B = 0b00_01_10_11 → reverse dwords
    check_parity({0x66, 0x0F, 0x70, 0xC1, 0x1B},
        [](CpuState& cpu) {
            cpu.sse.xmm[1].u32_[0] = 0xAAAAAAAA;
            cpu.sse.xmm[1].u32_[1] = 0xBBBBBBBB;
            cpu.sse.xmm[1].u32_[2] = 0xCCCCCCCC;
            cpu.sse.xmm[1].u32_[3] = 0xDDDDDDDD;
        },
        false, true);
}

// ============================================================================
// PE32-hot SSE2 MOVNT / MMX↔XMM / packed convert parity
// ============================================================================

TEST_F(JitParityTest, MovntdqStoreXmmToMem) {
    // MOVNTDQ [EBX], XMM0 — 66 0F E7 03. NT hint ignored; 128-bit store.
    check_parity({0x66, 0x0F, 0xE7, 0x03},
        [](CpuState& cpu) {
            cpu.gpr.set32(EBX, 0x3000);
            cpu.sse.xmm[0].u64_[0] = 0x0123456789ABCDEFull;
            cpu.sse.xmm[0].u64_[1] = 0xFEDCBA9876543210ull;
        },
        false, true);
}

TEST_F(JitParityTest, Movq2dqZerosUpperXmm) {
    // MOVQ2DQ XMM0, MM0 — F3 0F D6 C0. Low 64 from MM; upper 64 zero.
    check_parity({0xF3, 0x0F, 0xD6, 0xC0},
        [](CpuState& cpu) {
            cpu.fpu.set_mm(0, 0x0123456789ABCDEFull);
            cpu.sse.xmm[0].u64_[0] = 0xAAAAAAAAAAAAAAAAULL;
            cpu.sse.xmm[0].u64_[1] = 0xBBBBBBBBBBBBBBBBULL;
        },
        false, true);
}

TEST_F(JitParityTest, Cvtdq2pdRegReg) {
    // CVTDQ2PD XMM0, XMM1 — F3 0F E6 C1. Two i32 → two f64.
    check_parity({0xF3, 0x0F, 0xE6, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[1].i32_[0] = 42;
            cpu.sse.xmm[1].i32_[1] = -7;
            cpu.sse.xmm[1].u64_[1] = 0xDEADBEEFDEADBEEFULL;
            cpu.sse.xmm[0].u64_[0] = 0xAAAAAAAAAAAAAAAAULL;
            cpu.sse.xmm[0].u64_[1] = 0xBBBBBBBBBBBBBBBBULL;
        },
        false, true);
}

TEST_F(JitParityTest, Cvtpd2dqRegReg) {
    // CVTPD2DQ XMM0, XMM1 — F2 0F E6 C1. Two f64 → two i32; high 64 zero.
    check_parity({0xF2, 0x0F, 0xE6, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[1].f64_[0] = 1.0;
            cpu.sse.xmm[1].f64_[1] = -3.0;
            cpu.sse.xmm[0].u64_[0] = 0xAAAAAAAAAAAAAAAAULL;
            cpu.sse.xmm[0].u64_[1] = 0xBBBBBBBBBBBBBBBBULL;
        },
        false, true);
}

TEST_F(JitParityTest, Cvttpd2dqRegReg) {
    // CVTTPD2DQ XMM0, XMM1 — 66 0F E6 C1. Truncate two f64 → two i32.
    check_parity({0x66, 0x0F, 0xE6, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[1].f64_[0] = 1.9;
            cpu.sse.xmm[1].f64_[1] = -2.9;
            cpu.sse.xmm[0].u64_[0] = 0xAAAAAAAAAAAAAAAAULL;
            cpu.sse.xmm[0].u64_[1] = 0xBBBBBBBBBBBBBBBBULL;
        },
        false, true);
}

TEST_F(JitParityTest, Cvtpd2psRegReg) {
    // CVTPD2PS XMM0, XMM1 — 66 0F 5A C1. Two f64 → two f32; high 64 zero.
    check_parity({0x66, 0x0F, 0x5A, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[1].f64_[0] = 1.0;
            cpu.sse.xmm[1].f64_[1] = -4.0;
            cpu.sse.xmm[0].u64_[0] = 0xAAAAAAAAAAAAAAAAULL;
            cpu.sse.xmm[0].u64_[1] = 0xBBBBBBBBBBBBBBBBULL;
        },
        false, true);
}

TEST_F(JitParityTest, PabsbMemNative) {
    // 66 0F 38 1C 05 disp32 — PABSB xmm0, [0x2000] (REGONLY mem form)
    const u8 src[16] = {
        0x80, 0xFF, 0x01, 0x00, 0x7F, 0xFE, 0x02, 0x03,
        0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x0A,
    };
    ASSERT_TRUE(m_memory->write_bytes(0x2000, src, sizeof(src)).is_ok());
    check_parity({0x66, 0x0F, 0x38, 0x1C, 0x05, 0x00, 0x20, 0x00, 0x00},
        [](CpuState& cpu) {
            for (int i = 0; i < 16; ++i) {
                cpu.sse.xmm[0].u8_[i] = static_cast<u8>(0xA0 + i);
            }
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}
