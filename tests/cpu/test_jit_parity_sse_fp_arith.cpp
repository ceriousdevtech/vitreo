// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests (32-bit mode): scalar and packed SSE ADD/MUL/SUB
//
// Fixture and shared helpers: jit_parity_fixture.hpp.

#include "jit_parity_fixture.hpp"

// ============================================================================
// Scalar SSE ADD/MUL/SUB gated native (finite NEON + Inf/NaN helper)
// ============================================================================

TEST_F(JitParityTest, AddssFiniteNativeParity) {
    // F3 0F 58 C1 — ADDSS xmm0, xmm1 (finite → host FADD)
    check_parity({0xF3, 0x0F, 0x58, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[0].f32_[0] = 1.5f;
            cpu.sse.xmm[0].f32_[1] = 9.0f;
            cpu.sse.xmm[0].f32_[2] = 8.0f;
            cpu.sse.xmm[0].f32_[3] = 7.0f;
            cpu.sse.xmm[1].f32_[0] = 2.25f;
            cpu.sse.xmm[1].f32_[1] = 1.0f;
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParityTest, AddsdFiniteNativeParity) {
    // F2 0F 58 C1 — ADDSD xmm0, xmm1
    check_parity({0xF2, 0x0F, 0x58, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[0].f64_[0] = 1.5;
            cpu.sse.xmm[0].f64_[1] = 42.0;
            cpu.sse.xmm[1].f64_[0] = 2.25;
            cpu.sse.xmm[1].f64_[1] = 1.0;
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParityTest, MulssFiniteNativeParity) {
    // F3 0F 59 C1 — MULSS xmm0, xmm1
    check_parity({0xF3, 0x0F, 0x59, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[0].f32_[0] = 3.0f;
            cpu.sse.xmm[0].u32_[1] = 0x11111111u;
            cpu.sse.xmm[0].u32_[2] = 0x22222222u;
            cpu.sse.xmm[0].u32_[3] = 0x33333333u;
            cpu.sse.xmm[1].f32_[0] = 4.0f;
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParityTest, MulsdFiniteNativeParity) {
    // F2 0F 59 C1 — MULSD xmm0, xmm1
    check_parity({0xF2, 0x0F, 0x59, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[0].f64_[0] = 3.0;
            cpu.sse.xmm[0].u64_[1] = 0xAAAAAAAAAAAAAAAAULL;
            cpu.sse.xmm[1].f64_[0] = 4.0;
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParityTest, SubssFiniteNativeParity) {
    // F3 0F 5C C1 — SUBSS xmm0, xmm1
    check_parity({0xF3, 0x0F, 0x5C, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[0].f32_[0] = 5.0f;
            cpu.sse.xmm[0].f32_[1] = -1.0f;
            cpu.sse.xmm[1].f32_[0] = 2.0f;
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParityTest, SubsdFiniteNativeParity) {
    // F2 0F 5C C1 — SUBSD xmm0, xmm1
    check_parity({0xF2, 0x0F, 0x5C, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[0].f64_[0] = 5.0;
            cpu.sse.xmm[0].f64_[1] = -1.0;
            cpu.sse.xmm[1].f64_[0] = 2.0;
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParityTest, AddssMemFiniteNativeParity) {
    float src = 2.5f;
    u32 bits = 0;
    std::memcpy(&bits, &src, sizeof(bits));
    ASSERT_TRUE(m_memory->write_u32(0x3000, bits).is_ok());
    // F3 0F 58 03 — ADDSS xmm0, [ebx]
    check_parity({0xF3, 0x0F, 0x58, 0x03},
        [](CpuState& cpu) {
            cpu.gpr.set32(EBX, 0x3000);
            cpu.sse.xmm[0].f32_[0] = 1.0f;
            cpu.sse.xmm[0].f32_[1] = 6.0f;
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParityTest, AddssQNaNHelperParity) {
    // Destination QNaN must win (helper / sse_float_semantics).
    check_parity({0xF3, 0x0F, 0x58, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[0].u32_[0] = 0x7FC00001u;  // QNaN payload
            cpu.sse.xmm[0].u32_[1] = 0xDEADBEEFu;
            cpu.sse.xmm[1].f32_[0] = 1.0f;
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParityTest, MulssSrcQNaNHelperParity) {
    // Finite dst + QNaN src → src NaN via helper.
    check_parity({0xF3, 0x0F, 0x59, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[0].f32_[0] = 2.0f;
            cpu.sse.xmm[0].u32_[1] = 0xCAFEBABEu;
            cpu.sse.xmm[1].u32_[0] = 0x7FC00002u;
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParityTest, AddsdInfMinusInfIndefiniteHelper) {
    // +Inf + (-Inf) → indefinite QNaN (sign set) via helper.
    check_parity({0xF2, 0x0F, 0x58, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[0].f64_[0] =
                std::numeric_limits<double>::infinity();
            cpu.sse.xmm[0].u64_[1] = 0x0123456789ABCDEFULL;
            cpu.sse.xmm[1].f64_[0] =
                -std::numeric_limits<double>::infinity();
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParityTest, SubssBothNaNDstWinsHelper) {
    // QNaN dst + SNaN src → QNaN dst wins as-is.
    check_parity({0xF3, 0x0F, 0x5C, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[0].u32_[0] = 0x7FC00010u;  // QNaN
            cpu.sse.xmm[1].u32_[0] = 0x7F800001u;  // SNaN
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

// ============================================================================
// Packed SSE ADD/MUL/SUB gated native (finite NEON + Inf/NaN helper)
// ============================================================================

TEST_F(JitParityTest, AddpsFiniteNativeParity) {
    // 0F 58 C1 — ADDPS xmm0, xmm1
    check_parity({0x0F, 0x58, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[0].f32_[0] = 1.0f;
            cpu.sse.xmm[0].f32_[1] = 2.0f;
            cpu.sse.xmm[0].f32_[2] = 3.0f;
            cpu.sse.xmm[0].f32_[3] = 4.0f;
            cpu.sse.xmm[1].f32_[0] = 0.5f;
            cpu.sse.xmm[1].f32_[1] = 1.5f;
            cpu.sse.xmm[1].f32_[2] = 2.5f;
            cpu.sse.xmm[1].f32_[3] = 3.5f;
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParityTest, MulpsFiniteNativeParity) {
    // 0F 59 C1 — MULPS xmm0, xmm1
    check_parity({0x0F, 0x59, 0xC1},
        [](CpuState& cpu) {
            for (int i = 0; i < 4; ++i) {
                cpu.sse.xmm[0].f32_[i] = static_cast<float>(i + 1);
                cpu.sse.xmm[1].f32_[i] = 2.0f;
            }
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParityTest, SubpsFiniteNativeParity) {
    // 0F 5C C1 — SUBPS xmm0, xmm1
    check_parity({0x0F, 0x5C, 0xC1},
        [](CpuState& cpu) {
            for (int i = 0; i < 4; ++i) {
                cpu.sse.xmm[0].f32_[i] = 10.0f;
                cpu.sse.xmm[1].f32_[i] = static_cast<float>(i);
            }
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParityTest, AddpdFiniteNativeParity) {
    // 66 0F 58 C1 — ADDPD xmm0, xmm1
    check_parity({0x66, 0x0F, 0x58, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[0].f64_[0] = 1.5;
            cpu.sse.xmm[0].f64_[1] = 2.5;
            cpu.sse.xmm[1].f64_[0] = 0.5;
            cpu.sse.xmm[1].f64_[1] = 1.5;
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParityTest, MulpdFiniteNativeParity) {
    // 66 0F 59 C1 — MULPD xmm0, xmm1
    check_parity({0x66, 0x0F, 0x59, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[0].f64_[0] = 3.0;
            cpu.sse.xmm[0].f64_[1] = 4.0;
            cpu.sse.xmm[1].f64_[0] = 2.0;
            cpu.sse.xmm[1].f64_[1] = 0.5;
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParityTest, SubpdFiniteNativeParity) {
    // 66 0F 5C C1 — SUBPD xmm0, xmm1
    check_parity({0x66, 0x0F, 0x5C, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[0].f64_[0] = 5.0;
            cpu.sse.xmm[0].f64_[1] = 7.0;
            cpu.sse.xmm[1].f64_[0] = 2.0;
            cpu.sse.xmm[1].f64_[1] = 3.0;
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParityTest, AddpsQNaNLaneHelperParity) {
    // Lane 2 QNaN → whole-insn helper (destination-first quieting).
    check_parity({0x0F, 0x58, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[0].f32_[0] = 1.0f;
            cpu.sse.xmm[0].f32_[1] = 2.0f;
            cpu.sse.xmm[0].u32_[2] = 0x7FC00001u;
            cpu.sse.xmm[0].f32_[3] = 4.0f;
            cpu.sse.xmm[1].f32_[0] = 1.0f;
            cpu.sse.xmm[1].f32_[1] = 1.0f;
            cpu.sse.xmm[1].f32_[2] = 1.0f;
            cpu.sse.xmm[1].f32_[3] = 1.0f;
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParityTest, AddpsMaxFiniteStaysNativeParity) {
    // 0x7F7FFFFF is max finite (exp 0xFE). The Inf/NaN mask is 0x7F800000.
    check_parity({0x0F, 0x58, 0xC1},
        [](CpuState& cpu) {
            for (int i = 0; i < 4; ++i) {
                cpu.sse.xmm[0].u32_[i] = 0x7F7FFFFFu;
                cpu.sse.xmm[1].u32_[i] = 0x00800000u;  // min normal
            }
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParityTest, AddpsNegativeFiniteInfAndSnanGateParity) {
    check_parity({0x0F, 0x58, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[0].u32_[0] = 0xFF7FFFFFu;  // -max finite
            cpu.sse.xmm[0].u32_[1] = 0xFF800000u;  // -Inf
            cpu.sse.xmm[0].u32_[2] = 0xFF800001u;  // negative SNaN
            cpu.sse.xmm[0].u32_[3] = 0x80000000u;  // -0
            for (int lane = 0; lane < 4; ++lane) {
                cpu.sse.xmm[1].u32_[lane] = 0x00800000u;
            }
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParityTest, AddpsLane3QNaNHelperParity) {
    // High-half lane only. A W-register CBNZ on the OR-reduction would miss.
    check_parity({0x0F, 0x58, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[0].f32_[0] = 1.0f;
            cpu.sse.xmm[0].f32_[1] = 2.0f;
            cpu.sse.xmm[0].f32_[2] = 3.0f;
            cpu.sse.xmm[0].u32_[3] = 0x7FC00001u;
            cpu.sse.xmm[1].f32_[0] = 1.0f;
            cpu.sse.xmm[1].f32_[1] = 1.0f;
            cpu.sse.xmm[1].f32_[2] = 1.0f;
            cpu.sse.xmm[1].f32_[3] = 1.0f;
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParityTest, MulpsSrcInfHelperParity) {
    check_parity({0x0F, 0x59, 0xC1},
        [](CpuState& cpu) {
            for (int i = 0; i < 4; ++i) {
                cpu.sse.xmm[0].f32_[i] = 2.0f;
                cpu.sse.xmm[1].f32_[i] = 3.0f;
            }
            cpu.sse.xmm[1].f32_[1] =
                std::numeric_limits<float>::infinity();
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParityTest, AddpdInfMinusInfIndefiniteHelper) {
    // +Inf + (-Inf) → indefinite QNaN via helper.
    check_parity({0x66, 0x0F, 0x58, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[0].f64_[0] =
                std::numeric_limits<double>::infinity();
            cpu.sse.xmm[0].f64_[1] = 1.0;
            cpu.sse.xmm[1].f64_[0] =
                -std::numeric_limits<double>::infinity();
            cpu.sse.xmm[1].f64_[1] = 2.0;
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParityTest, AddpsMemFiniteNativeParity) {
    float lanes[4] = {0.5f, 1.5f, 2.5f, 3.5f};
    for (int i = 0; i < 4; ++i) {
        u32 bits = 0;
        std::memcpy(&bits, &lanes[i], sizeof(bits));
        ASSERT_TRUE(m_memory->write_u32(
            static_cast<u64>(0x3000 + i * 4), bits).is_ok());
    }
    // 0F 58 03 — ADDPS xmm0, [ebx]
    check_parity({0x0F, 0x58, 0x03},
        [](CpuState& cpu) {
            cpu.gpr.set32(EBX, 0x3000);
            cpu.sse.xmm[0].f32_[0] = 1.0f;
            cpu.sse.xmm[0].f32_[1] = 2.0f;
            cpu.sse.xmm[0].f32_[2] = 3.0f;
            cpu.sse.xmm[0].f32_[3] = 4.0f;
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}
