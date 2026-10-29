// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests (32-bit mode): SSE conversions and PEXTR/EXTRACTPS to memory
//
// Fixture and shared helpers: jit_parity_fixture.hpp.

#include "jit_parity_fixture.hpp"

// ============================================================================
// PE32 SSE: CVTPI2PS / CVTPS2PI / EXTRACTPS·PEXTR mem / PMADDUBSW
// ============================================================================

TEST_F(JitParityTest, Cvtpi2psMmToXmmPreservesHigh) {
    // CVTPI2PS XMM0, MM1 — 0F 2A C1. Low two f32 from MM; high XMM unchanged.
    check_parity({0x0F, 0x2A, 0xC1},
        [](CpuState& cpu) {
            cpu.fpu.set_mm(1, (static_cast<u64>(static_cast<u32>(-7)) << 32) | 42u);
            cpu.sse.xmm[0].f32_[2] = 88.0f;
            cpu.sse.xmm[0].f32_[3] = 77.0f;
            cpu.sse.xmm[0].u64_[0] = 0xAAAAAAAAAAAAAAAAULL;
        },
        false, true);
}

TEST_F(JitParityTest, Cvtps2piXmmToMm) {
    // CVTPS2PI MM0, XMM1 ; MOVQ [EBX], MM0 — round then spill for mem compare.
    check_parity({0x0F, 0x2D, 0xC1, 0x0F, 0x7F, 0x03},
        [](CpuState& cpu) {
            cpu.gpr.set32(EBX, 0x3000);
            cpu.sse.xmm[1].f32_[0] = 3.7f;
            cpu.sse.xmm[1].f32_[1] = -2.5f;
            cpu.fpu.set_mm(0, 0);
        },
        false, true);
}

TEST_F(JitParityTest, Cvttps2piXmmToMm) {
    // CVTTPS2PI MM0, XMM1 ; MOVQ [EBX], MM0 — truncate then spill.
    check_parity({0x0F, 0x2C, 0xC1, 0x0F, 0x7F, 0x03},
        [](CpuState& cpu) {
            cpu.gpr.set32(EBX, 0x3000);
            cpu.sse.xmm[1].f32_[0] = 3.7f;
            cpu.sse.xmm[1].f32_[1] = -2.5f;
            cpu.fpu.set_mm(0, 0);
        },
        false, true);
}

TEST_F(JitParityTest, Cvtpi2pdMmToXmm) {
    // CVTPI2PD XMM0, MM1 — 66 0F 2A C1. Two i32 → two f64.
    check_parity({0x66, 0x0F, 0x2A, 0xC1},
        [](CpuState& cpu) {
            cpu.fpu.set_mm(1, (static_cast<u64>(static_cast<u32>(-9)) << 32) | 5u);
            cpu.sse.xmm[0].u64_[0] = 0xAAAAAAAAAAAAAAAAULL;
            cpu.sse.xmm[0].u64_[1] = 0xBBBBBBBBBBBBBBBBULL;
        },
        false, true);
}

TEST_F(JitParityTest, Cvtpd2piXmmToMm) {
    // CVTPD2PI MM0, XMM1 ; MOVQ [EBX], MM0 — round then spill.
    check_parity({0x66, 0x0F, 0x2D, 0xC1, 0x0F, 0x7F, 0x03},
        [](CpuState& cpu) {
            cpu.gpr.set32(EBX, 0x3000);
            cpu.sse.xmm[1].f64_[0] = 3.7;
            cpu.sse.xmm[1].f64_[1] = -2.5;
            cpu.fpu.set_mm(0, 0);
        },
        false, true);
}

TEST_F(JitParityTest, Cvttpd2piXmmToMm) {
    // CVTTPD2PI MM0, XMM1 ; MOVQ [EBX], MM0 — truncate then spill.
    check_parity({0x66, 0x0F, 0x2C, 0xC1, 0x0F, 0x7F, 0x03},
        [](CpuState& cpu) {
            cpu.gpr.set32(EBX, 0x3000);
            cpu.sse.xmm[1].f64_[0] = 6.9;
            cpu.sse.xmm[1].f64_[1] = -6.9;
            cpu.fpu.set_mm(0, 0);
        },
        false, true);
}

TEST_F(JitParityTest, Cvtpd2piIndefiniteOnOverflow) {
    // CVTPD2PI MM0, XMM1 ; MOVQ [EBX], MM0 — overflow → 0x80000000.
    check_parity({0x66, 0x0F, 0x2D, 0xC1, 0x0F, 0x7F, 0x03},
        [](CpuState& cpu) {
            cpu.gpr.set32(EBX, 0x3000);
            cpu.sse.xmm[1].f64_[0] = 1.0e20;
            cpu.sse.xmm[1].f64_[1] = std::numeric_limits<double>::quiet_NaN();
            cpu.fpu.set_mm(0, 0);
        },
        false, true);
}

TEST_F(JitParityTest, NativeCvtsi2sdEcxPreservesHighQword) {
    // CVTSI2SD XMM0, ECX — F2 0F 2A C1
    check_parity({0xF2, 0x0F, 0x2A, 0xC1},
        [](CpuState& cpu) {
            cpu.gpr.set32(ECX, -42);
            cpu.sse.xmm[0].u64_[0] = 0xAAAAAAAAAAAAAAAAULL;
            cpu.sse.xmm[0].u64_[1] = 0xBBBBBBBBBBBBBBBBULL;
        },
        true, true);
}

TEST_F(JitParityTest, NativeCvttss2siAndCvtss2si) {
    // CVTTSS2SI EAX, XMM1 ; CVTSS2SI ECX, XMM1 — F3 0F 2C C1 / F3 0F 2D C9
    check_parity({0xF3, 0x0F, 0x2C, 0xC1, 0xF3, 0x0F, 0x2D, 0xC9},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xDEADBEEFu);
            cpu.gpr.set32(ECX, 0xCAFEBABEu);
            cpu.sse.xmm[1].f32_[0] = 3.7f;
            cpu.sse.xmm[1].u32_[1] = 0x11111111u;
        },
        true, true);
}

TEST_F(JitParityTest, NativeCvttss2siIndefiniteOnOverflow) {
    // CVTTSS2SI EAX, XMM1 — overflow / NaN → 0x80000000
    check_parity({0xF3, 0x0F, 0x2C, 0xC1},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0);
            cpu.sse.xmm[1].f32_[0] = 1.0e20f;
        },
        true, true);
    check_parity({0xF3, 0x0F, 0x2C, 0xC1},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0);
            cpu.sse.xmm[1].f32_[0] = std::numeric_limits<float>::quiet_NaN();
        },
        true, true);
}

TEST_F(JitParityTest, NativeCvttsd2siAndCvtsd2si) {
    // CVTTSD2SI EAX, XMM1 ; CVTSD2SI ECX, XMM1 — F2 0F 2C C1 / F2 0F 2D C9
    check_parity({0xF2, 0x0F, 0x2C, 0xC1, 0xF2, 0x0F, 0x2D, 0xC9},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xDEADBEEFu);
            cpu.gpr.set32(ECX, 0xCAFEBABEu);
            cpu.sse.xmm[1].f64_[0] = -3.7;
            cpu.sse.xmm[1].u64_[1] = 0x1111111111111111ULL;
        },
        true, true);
}

TEST_F(JitParityTest, NativeCvtss2sdPreservesHigh) {
    // CVTSS2SD XMM0, XMM1 — F3 0F 5A C1
    check_parity({0xF3, 0x0F, 0x5A, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[1].f32_[0] = 1.5f;
            cpu.sse.xmm[0].u64_[0] = 0xAAAAAAAAAAAAAAAAULL;
            cpu.sse.xmm[0].u64_[1] = 0xBBBBBBBBBBBBBBBBULL;
        },
        false, true);
}

TEST_F(JitParityTest, NativeCvtsd2ssPreservesUpper) {
    // CVTSD2SS XMM0, XMM1 — F2 0F 5A C1
    check_parity({0xF2, 0x0F, 0x5A, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[1].f64_[0] = 2.5;
            cpu.sse.xmm[0].u32_[0] = 0x3F800000u;
            cpu.sse.xmm[0].u32_[1] = 0x40000000u;
            cpu.sse.xmm[0].u32_[2] = 0x40400000u;
            cpu.sse.xmm[0].u32_[3] = 0x40800000u;
        },
        false, true);
}

TEST_F(JitParityTest, NativeCvtdq2psRegReg) {
    // CVTDQ2PS XMM0, XMM1 — 0F 5B C1
    check_parity({0x0F, 0x5B, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[1].i32_[0] = 42;
            cpu.sse.xmm[1].i32_[1] = -7;
            cpu.sse.xmm[1].i32_[2] = 1000000;
            cpu.sse.xmm[1].i32_[3] = -1000000;
            cpu.sse.xmm[0].u64_[0] = 0xAAAAAAAAAAAAAAAAULL;
            cpu.sse.xmm[0].u64_[1] = 0xBBBBBBBBBBBBBBBBULL;
        },
        false, true);
}

TEST_F(JitParityTest, NativeCvtps2dqAndCvttps2dq) {
    // CVTPS2DQ XMM0, XMM1 ; CVTTPS2DQ XMM2, XMM1
    check_parity({0x66, 0x0F, 0x5B, 0xC1, 0xF3, 0x0F, 0x5B, 0xD1},
        [](CpuState& cpu) {
            cpu.sse.xmm[1].f32_[0] = 1.5f;
            cpu.sse.xmm[1].f32_[1] = -2.7f;
            cpu.sse.xmm[1].f32_[2] = 1.0e20f;  // indefinite
            cpu.sse.xmm[1].f32_[3] = std::numeric_limits<float>::quiet_NaN();
            cpu.sse.xmm[0].u64_[0] = 0xAAAAAAAAAAAAAAAAULL;
            cpu.sse.xmm[0].u64_[1] = 0xBBBBBBBBBBBBBBBBULL;
            cpu.sse.xmm[2].u64_[0] = 0xCCCCCCCCCCCCCCCCULL;
            cpu.sse.xmm[2].u64_[1] = 0xDDDDDDDDDDDDDDDDULL;
        },
        false, true);
}

TEST_F(JitParityTest, NativePmuludqRegReg) {
    // PMULUDQ XMM0, XMM1 — 66 0F F4 C1 (even dword × even dword → qword)
    check_parity({0x66, 0x0F, 0xF4, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[0].u32_[0] = 0xFFFFFFFFu;
            cpu.sse.xmm[0].u32_[1] = 0x11111111u;
            cpu.sse.xmm[0].u32_[2] = 0x00010000u;
            cpu.sse.xmm[0].u32_[3] = 0x22222222u;
            cpu.sse.xmm[1].u32_[0] = 0xFFFFFFFFu;
            cpu.sse.xmm[1].u32_[1] = 0x33333333u;
            cpu.sse.xmm[1].u32_[2] = 0x00020000u;
            cpu.sse.xmm[1].u32_[3] = 0x44444444u;
        },
        false, true);
}

TEST_F(JitParityTest, NativeRcpssAndRsqrtssPreserveUpper) {
    // RCPSS XMM0, XMM1 ; RSQRTSS XMM2, XMM1 — F3 0F 53 C1 / F3 0F 52 D1
    check_parity({0xF3, 0x0F, 0x53, 0xC1, 0xF3, 0x0F, 0x52, 0xD1},
        [](CpuState& cpu) {
            cpu.sse.xmm[1].f32_[0] = 4.0f;
            cpu.sse.xmm[0].u32_[0] = 0x3F800000u;
            cpu.sse.xmm[0].u32_[1] = 0x40000000u;
            cpu.sse.xmm[0].u32_[2] = 0x40400000u;
            cpu.sse.xmm[0].u32_[3] = 0x40800000u;
            cpu.sse.xmm[2].u32_[0] = 0x3F000000u;
            cpu.sse.xmm[2].u32_[1] = 0x3F400000u;
            cpu.sse.xmm[2].u32_[2] = 0x3F600000u;
            cpu.sse.xmm[2].u32_[3] = 0x3F700000u;
        },
        false, true);
}

TEST_F(JitParityTest, ExtractpsToMem) {
    // EXTRACTPS [EBX], XMM0, 1 — 66 0F 3A 17 03 01.
    check_parity({0x66, 0x0F, 0x3A, 0x17, 0x03, 0x01},
        [](CpuState& cpu) {
            cpu.gpr.set32(EBX, 0x3000);
            cpu.sse.xmm[0].u32_[0] = 0x11111111u;
            cpu.sse.xmm[0].u32_[1] = 0x22222222u;
            cpu.sse.xmm[0].u32_[2] = 0x33333333u;
            cpu.sse.xmm[0].u32_[3] = 0x44444444u;
        },
        false, true);
}

TEST_F(JitParityTest, PextrdToMem) {
    // PEXTRD [EBX], XMM0, 2 — 66 0F 3A 16 03 02.
    check_parity({0x66, 0x0F, 0x3A, 0x16, 0x03, 0x02},
        [](CpuState& cpu) {
            cpu.gpr.set32(EBX, 0x3000);
            cpu.sse.xmm[0].u32_[0] = 0xAAAAu;
            cpu.sse.xmm[0].u32_[1] = 0xBBBBu;
            cpu.sse.xmm[0].u32_[2] = 0xCCCCu;
            cpu.sse.xmm[0].u32_[3] = 0xDDDDu;
        },
        false, true);
}

TEST_F(JitParityTest, PextrbToMem) {
    // PEXTRB [EBX], XMM0, 5 — 66 0F 3A 14 03 05.
    check_parity({0x66, 0x0F, 0x3A, 0x14, 0x03, 0x05},
        [](CpuState& cpu) {
            cpu.gpr.set32(EBX, 0x3000);
            for (int i = 0; i < 16; ++i) {
                cpu.sse.xmm[0].u8_[i] = static_cast<u8>(0xA0 + i);
            }
        },
        false, true);
}

TEST_F(JitParityTest, PmaddubswXmmRegReg) {
    // PMADDUBSW XMM0, XMM1 — 66 0F 38 04 C1.
    check_parity({0x66, 0x0F, 0x38, 0x04, 0xC1},
        [](CpuState& cpu) {
            // Unsigned dst bytes; signed src bytes. Pair products saturate to i16.
            for (int i = 0; i < 16; ++i) {
                cpu.sse.xmm[0].u8_[i] = static_cast<u8>(200 + (i & 3));
                cpu.sse.xmm[1].i8_[i] = static_cast<i8>((i & 1) ? -40 : 30);
            }
        },
        false, true);
}
