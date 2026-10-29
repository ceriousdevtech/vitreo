// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests — 64-bit long mode (CPU-048): FMA and the
// VEX.256 integer lane split.

#include "jit_parity_64_fixture.hpp"

// ============================================================================
// FMA — VEX.256 lane split and memory operands
//
// The FMA lowering was converted to vec_off() and the packed FMA opcodes added
// to vex256_lane_split_allowed() on 2026-08-22, but shipped with NO tests. An
// untested FP lowering is the worst thing to leave in the tree, so these pin
// the architectural answer directly rather than only checking JIT/interpreter
// parity: both engines can be wrong identically (see the VCMPPS `& 7`
// predicate-masking bug and the FNMADD NaN-sign defect, both live today).
//
// Operand naming, from the assembler's own annotation (`objdump` prints e.g.
// "ymm0 = (ymm1 * ymm0) + ymm2"), with a=ymm0 (dst AND a source), b=ymm1
// (vvvv), c=ymm2 (r/m):
//     132:  a = a*c + b
//     213:  a = b*a + c
//     231:  a = b*c + a
// Seeds are chosen so all three forms and both lanes give distinct answers:
// low lane a=2 b=3 c=5, high lane a=7 b=11 c=13.
//     213 -> low 3*2+5 = 11    high 11*7+13 = 90
//     231 -> low 3*5+2 = 17    high 11*13+7 = 150
//     132 -> low 2*5+3 = 13    high  7*13+11 = 102
// A lane mix-up, or a transposed 213/231, lands on none of these.
// ============================================================================

namespace {

void seed_fma_ymm(CpuState& cpu) {
    for (int i = 0; i < 4; ++i) {
        cpu.sse.xmm[0].f32_[i] = 2.0f;
        cpu.sse.xmm[1].f32_[i] = 3.0f;
        cpu.sse.xmm[2].f32_[i] = 5.0f;
        cpu.avx.ymm_high[0].f32_[i] = 7.0f;
        cpu.avx.ymm_high[1].f32_[i] = 11.0f;
        cpu.avx.ymm_high[2].f32_[i] = 13.0f;
    }
}

}  // namespace

TEST_F(JitParity64Test, Vfmadd213Ps256WritesBothLanes) {
    // c4 e2 75 a8 c2 — vfmadd213ps ymm0, ymm1, ymm2
    static const u8 kCode[] = {0xC4, 0xE2, 0x75, 0xA8, 0xC2};
    BasicBlock block = build_block(kCode, sizeof(kCode));
    ASSERT_TRUE(block.instructions[0].prefixes.vex_l);

    check_native_parity({0xC4, 0xE2, 0x75, 0xA8, 0xC2}, seed_fma_ymm,
                        /*check_flags=*/false, /*check_xmm=*/true);

    CpuState icpu;
    init_cpu(icpu);
    seed_fma_ymm(icpu);
    ASSERT_TRUE(m_memory->write_bytes(0x1000, kCode, sizeof(kCode)).is_ok());
    {
        Interpreter interp(icpu, *m_memory);
        interp.run(1);
    }
    for (int i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(icpu.sse.xmm[0].f32_[i], 11.0f) << "low lane " << i;
        EXPECT_FLOAT_EQ(icpu.avx.ymm_high[0].f32_[i], 90.0f) << "high lane " << i;
    }
}

TEST_F(JitParity64Test, Vfmadd231Ps256IsNotVfmadd213) {
    // c4 e2 75 b8 c2 — vfmadd231ps ymm0, ymm1, ymm2. Distinct from 213 in
    // BOTH lanes (17/150 vs 11/90), so a transposition cannot hide.
    static const u8 kCode[] = {0xC4, 0xE2, 0x75, 0xB8, 0xC2};
    check_native_parity({0xC4, 0xE2, 0x75, 0xB8, 0xC2}, seed_fma_ymm,
                        /*check_flags=*/false, /*check_xmm=*/true);

    CpuState icpu;
    init_cpu(icpu);
    seed_fma_ymm(icpu);
    ASSERT_TRUE(m_memory->write_bytes(0x1000, kCode, sizeof(kCode)).is_ok());
    {
        Interpreter interp(icpu, *m_memory);
        interp.run(1);
    }
    for (int i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(icpu.sse.xmm[0].f32_[i], 17.0f);
        EXPECT_FLOAT_EQ(icpu.avx.ymm_high[0].f32_[i], 150.0f);
    }
}

TEST_F(JitParity64Test, Vfmadd132Ps256IsNotVfmadd213Or231) {
    // c4 e2 75 98 c2 — vfmadd132ps ymm0, ymm1, ymm2 -> 13 / 102
    static const u8 kCode[] = {0xC4, 0xE2, 0x75, 0x98, 0xC2};
    check_native_parity({0xC4, 0xE2, 0x75, 0x98, 0xC2}, seed_fma_ymm,
                        /*check_flags=*/false, /*check_xmm=*/true);

    CpuState icpu;
    init_cpu(icpu);
    seed_fma_ymm(icpu);
    ASSERT_TRUE(m_memory->write_bytes(0x1000, kCode, sizeof(kCode)).is_ok());
    {
        Interpreter interp(icpu, *m_memory);
        interp.run(1);
    }
    for (int i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(icpu.sse.xmm[0].f32_[i], 13.0f);
        EXPECT_FLOAT_EQ(icpu.avx.ymm_high[0].f32_[i], 102.0f);
    }
}

// The negated forms are the ones worth pinning by execution: ARM's "N"
// negates the WHOLE expression while x86's negates only the product, so the
// ARM pairing is crosswise (VFMSUB<->FNMSUB, VFNMSUB<->FNMADD). Getting it
// backwards produces a sign error, not a crash.
//   213 seeds, low lane a=2 b=3 c=5:
//     vfnmadd213ps  -(b*a) + c = -1        high: -(11*7)+13 = -64
//     vfmsub213ps    (b*a) - c =  1        high:  (11*7)-13 =  64
//     vfnmsub213ps  -(b*a) - c = -11       high: -(11*7)-13 = -90
TEST_F(JitParity64Test, Vfnmadd213Ps256NegatesOnlyTheProduct) {
    static const u8 kCode[] = {0xC4, 0xE2, 0x75, 0xAC, 0xC2};
    check_native_parity({0xC4, 0xE2, 0x75, 0xAC, 0xC2}, seed_fma_ymm,
                        /*check_flags=*/false, /*check_xmm=*/true);

    CpuState icpu;
    init_cpu(icpu);
    seed_fma_ymm(icpu);
    ASSERT_TRUE(m_memory->write_bytes(0x1000, kCode, sizeof(kCode)).is_ok());
    {
        Interpreter interp(icpu, *m_memory);
        interp.run(1);
    }
    for (int i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(icpu.sse.xmm[0].f32_[i], -1.0f);
        EXPECT_FLOAT_EQ(icpu.avx.ymm_high[0].f32_[i], -64.0f);
    }
}

TEST_F(JitParity64Test, Vfmsub213Ps256SubtractsTheAddend) {
    static const u8 kCode[] = {0xC4, 0xE2, 0x75, 0xAA, 0xC2};
    check_native_parity({0xC4, 0xE2, 0x75, 0xAA, 0xC2}, seed_fma_ymm,
                        /*check_flags=*/false, /*check_xmm=*/true);

    CpuState icpu;
    init_cpu(icpu);
    seed_fma_ymm(icpu);
    ASSERT_TRUE(m_memory->write_bytes(0x1000, kCode, sizeof(kCode)).is_ok());
    {
        Interpreter interp(icpu, *m_memory);
        interp.run(1);
    }
    for (int i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(icpu.sse.xmm[0].f32_[i], 1.0f);
        EXPECT_FLOAT_EQ(icpu.avx.ymm_high[0].f32_[i], 64.0f);
    }
}

TEST_F(JitParity64Test, Vfnmsub213Ps256NegatesProductAndSubtracts) {
    static const u8 kCode[] = {0xC4, 0xE2, 0x75, 0xAE, 0xC2};
    check_native_parity({0xC4, 0xE2, 0x75, 0xAE, 0xC2}, seed_fma_ymm,
                        /*check_flags=*/false, /*check_xmm=*/true);

    CpuState icpu;
    init_cpu(icpu);
    seed_fma_ymm(icpu);
    ASSERT_TRUE(m_memory->write_bytes(0x1000, kCode, sizeof(kCode)).is_ok());
    {
        Interpreter interp(icpu, *m_memory);
        interp.run(1);
    }
    for (int i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(icpu.sse.xmm[0].f32_[i], -11.0f);
        EXPECT_FLOAT_EQ(icpu.avx.ymm_high[0].f32_[i], -90.0f);
    }
}

// The memory form ABOVE 4 GiB. Every older oracle test ran at 0x04000000, so
// an effective address marshalled through a 32-bit MOV was silently correct
// there and corrupt here -- a bug family with four recorded instances. The
// lane-1 pass adds +16 to the displacement, so this also proves the high lane
// reads the SECOND 16 bytes rather than re-reading the first.
TEST_F(JitParity64Test, Vfmadd213Ps256MemoryOperandAbove4GiB) {
    constexpr GuestAddr kData = 0x140006000ull;
    map_tracked_region(kData, 0x1000);

    // c4 e2 75 a8 03 — vfmadd213ps ymm0, ymm1, [rbx]
    static const u8 kCode[] = {0xC4, 0xE2, 0x75, 0xA8, 0x03};

    auto setup = [](CpuState& cpu) {
        seed_fma_ymm(cpu);
        cpu.gpr.set64(RBX, kData);
    };

    // c = 5.0 in the low 16 bytes, 13.0 in the high 16 -> same expectations
    // as the register test: 11 low, 90 high.
    float mem[8];
    for (int i = 0; i < 4; ++i) {
        mem[i] = 5.0f;
        mem[4 + i] = 13.0f;
    }
    ASSERT_TRUE(m_memory->write_bytes(kData, reinterpret_cast<const u8*>(mem),
                                      sizeof(mem)).is_ok());

    check_native_parity({0xC4, 0xE2, 0x75, 0xA8, 0x03}, setup,
                        /*check_flags=*/false, /*check_xmm=*/true);

    CpuState icpu;
    init_cpu(icpu);
    setup(icpu);
    ASSERT_TRUE(m_memory->write_bytes(kData, reinterpret_cast<const u8*>(mem),
                                      sizeof(mem)).is_ok());
    ASSERT_TRUE(m_memory->write_bytes(0x1000, kCode, sizeof(kCode)).is_ok());
    {
        Interpreter interp(icpu, *m_memory);
        interp.run(1);
    }
    for (int i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(icpu.sse.xmm[0].f32_[i], 11.0f);
        EXPECT_FLOAT_EQ(icpu.avx.ymm_high[0].f32_[i], 90.0f) << "high lane read "
            << "the wrong 16 bytes (lane-1 disp+16)";
    }
}

// VEX.128 FMA must ZERO ymm[255:128]. Stale-high-lane bugs of exactly this
// shape made an AVX2 strlen invent matches in bytes 16..31.
TEST_F(JitParity64Test, Vfmadd213Ps128ZeroesHighLane) {
    static const u8 kCode[] = {0xC4, 0xE2, 0x71, 0xA8, 0xC2};
    BasicBlock block = build_block(kCode, sizeof(kCode));
    ASSERT_FALSE(block.instructions[0].prefixes.vex_l);

    check_native_parity({0xC4, 0xE2, 0x71, 0xA8, 0xC2}, seed_fma_ymm,
                        /*check_flags=*/false, /*check_xmm=*/true);

    CpuState icpu;
    init_cpu(icpu);
    seed_fma_ymm(icpu);
    ASSERT_TRUE(m_memory->write_bytes(0x1000, kCode, sizeof(kCode)).is_ok());
    {
        Interpreter interp(icpu, *m_memory);
        interp.run(1);
    }
    for (int i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(icpu.sse.xmm[0].f32_[i], 11.0f);
    }
    EXPECT_EQ(icpu.avx.ymm_high[0].u64_[0], 0ull);
    EXPECT_EQ(icpu.avx.ymm_high[0].u64_[1], 0ull);
}


// ============================================================================
// VEX.256 lane split — the EMIT_SSE_NEON_RR family
//
// These run the 128-bit lowering twice, once per lane, so the failure mode to
// catch is a lane mix-up: every YMM is seeded with a DIFFERENT pattern in its
// low and high lane and the sources are asymmetric. check_native_parity also
// sets VITREO_JIT_NO_DELEGATE and requires the JIT to have lowered the block,
// so an op that silently stops being lane-split — and goes back to costing an
// interpreter round trip per instruction — fails here instead of quietly
// regressing performance.
//
// Contract source: Intel SDM Vol. 2, "VEX.256 encoded version:
// DEST[255:128] <- SRC1[255:128] OP SRC2[255:128]" for each of these.
// ============================================================================

namespace {

// ymm1 = SRC1, ymm2 = SRC2, ymm0 = DEST seeded with junk that must be gone.
void seed_vex256_operands(CpuState& cpu) {
    for (int b = 0; b < 16; ++b) {
        cpu.sse.xmm[1].u8_[b] = static_cast<u8>(0x10 + b);
        cpu.avx.ymm_high[1].u8_[b] = static_cast<u8>(0xA0 + b);
        cpu.sse.xmm[2].u8_[b] = static_cast<u8>(0x7D - b * 3);
        cpu.avx.ymm_high[2].u8_[b] = static_cast<u8>(0x31 + b * 5);
        cpu.sse.xmm[0].u8_[b] = 0xDE;
        cpu.avx.ymm_high[0].u8_[b] = 0xAD;
    }
}

}  // namespace

// VEX.256.66.0F <op> /r — vXXX ymm0, ymm1, ymm2  (C5 F5 op C2)
#define VITREO_VEX256_66_0F_PARITY(TestName, OpByte)                    \
    TEST_F(JitParity64Test, TestName) {                                 \
        check_native_parity({0xC5, 0xF5, (OpByte), 0xC2},               \
                            seed_vex256_operands,                       \
                            /*check_flags=*/false, /*check_xmm=*/true); \
    }

VITREO_VEX256_66_0F_PARITY(Vex256PunpcklwdLaneSplit, 0x61)
VITREO_VEX256_66_0F_PARITY(Vex256PunpckhwdLaneSplit, 0x69)
VITREO_VEX256_66_0F_PARITY(Vex256PmaddwdLaneSplit,   0xF5)

TEST_F(JitParity64Test, Vex256WordOperationsAliasedDestination) {
    for (u8 opcode : {u8{0x61}, u8{0x69}, u8{0xF5}}) {
        for (u8 dest : {u8{1}, u8{2}}) {
            check_native_parity({0xC5, 0xF5, opcode,
                                 static_cast<u8>(0xC2 | (dest << 3))},
                                seed_vex256_operands, true, true);
        }
    }
}

TEST_F(JitParity64Test, Vex256PmaddwdSignedOverflowBothLanes) {
    check_native_parity({0xC5, 0xF5, 0xF5, 0xC2}, [](CpuState& cpu) {
        for (int i = 0; i < 8; ++i) {
            cpu.sse.xmm[1].u16_[i] = cpu.sse.xmm[2].u16_[i] = 0x8000;
            cpu.avx.ymm_high[1].u16_[i] = 0xFFFF; // -1
            cpu.avx.ymm_high[2].u16_[i] = 3;
        }
    }, true, true);
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(m_last_jit_cpu.sse.xmm[0].u32_[i], 0x80000000u);
        EXPECT_EQ(m_last_jit_cpu.avx.ymm_high[0].u32_[i], 0xFFFFFFFAu);
    }
}

TEST_F(JitParity64Test, Vex256PmaddwdUnalignedMemoryAndDestinationAlias) {
    u16 words[16];
    for (int i = 0; i < 16; ++i) words[i] = static_cast<u16>(i + 1);
    ASSERT_TRUE(m_memory->write_bytes(0x8003,
        reinterpret_cast<const u8*>(words), sizeof(words)));
    // vpmaddwd ymm1, ymm1, [rax]
    check_native_parity({0xC5, 0xF5, 0xF5, 0x08}, [](CpuState& cpu) {
        cpu.gpr.rax = 0x8003;
        for (int i = 0; i < 8; ++i) {
            cpu.sse.xmm[1].u16_[i] = 0xFFFF;
            cpu.avx.ymm_high[1].u16_[i] = 0xFFFF;
        }
    }, true, true);
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(m_last_jit_cpu.sse.xmm[1].u32_[i], static_cast<u32>(-(4 * i + 3)));
        EXPECT_EQ(m_last_jit_cpu.avx.ymm_high[1].u32_[i], static_cast<u32>(-(4 * i + 19)));
    }
}

TEST_F(JitParity64Test, Vex256PmaddwdMemoryFaultPreservesEntireDestination) {
    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    const u8 code[] = {0xC5, 0xF5, 0xF5, 0x08};
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    BasicBlock block = build_block(code, sizeof(code));
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    for (u64 readable : {0u, 8u, 16u, 24u}) {
        CpuState cpu;
        init_cpu(cpu);
        cpu.gpr.rax = 0x11000 - readable; // map ends at 0x11000
        for (int i = 0; i < 2; ++i) {
            cpu.sse.xmm[1].u64_[i] = 0x1122334455667788ull;
            cpu.avx.ymm_high[1].u64_[i] = 0x8877665544332211ull;
        }
        CpuState interpreted = cpu;
        Interpreter interpreter(interpreted, *m_memory);
        EXPECT_EQ(interpreter.step(), ExecResult::Exception);
        for (int i = 0; i < 2; ++i) {
            EXPECT_EQ(interpreted.sse.xmm[1].u64_[i], 0x1122334455667788ull);
            EXPECT_EQ(interpreted.avx.ymm_high[1].u64_[i], 0x8877665544332211ull);
        }
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
        ASSERT_TRUE(cpu.pending_raise.active);
        EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
        for (int i = 0; i < 2; ++i) {
            EXPECT_EQ(cpu.sse.xmm[1].u64_[i], 0x1122334455667788ull);
            EXPECT_EQ(cpu.avx.ymm_high[1].u64_[i], 0x8877665544332211ull);
        }
    }
}

VITREO_VEX256_66_0F_PARITY(Vex256PaddbLaneSplit,   0xFC)
VITREO_VEX256_66_0F_PARITY(Vex256PaddwLaneSplit,   0xFD)
VITREO_VEX256_66_0F_PARITY(Vex256PaddqLaneSplit,   0xD4)
VITREO_VEX256_66_0F_PARITY(Vex256PsubbLaneSplit,   0xF8)
VITREO_VEX256_66_0F_PARITY(Vex256PsubwLaneSplit,   0xF9)
VITREO_VEX256_66_0F_PARITY(Vex256PaddusbLaneSplit, 0xDC)
VITREO_VEX256_66_0F_PARITY(Vex256PaddaswLaneSplit, 0xDD)
VITREO_VEX256_66_0F_PARITY(Vex256PsubusbLaneSplit, 0xD8)
VITREO_VEX256_66_0F_PARITY(Vex256PsubuswLaneSplit, 0xD9)
VITREO_VEX256_66_0F_PARITY(Vex256PmullwLaneSplit,  0xD5)
VITREO_VEX256_66_0F_PARITY(Vex256PcmpeqbLaneSplit, 0x74)
VITREO_VEX256_66_0F_PARITY(Vex256PcmpeqwLaneSplit, 0x75)
VITREO_VEX256_66_0F_PARITY(Vex256PcmpeqdLaneSplit, 0x76)
VITREO_VEX256_66_0F_PARITY(Vex256PcmpgtbLaneSplit, 0x64)
VITREO_VEX256_66_0F_PARITY(Vex256PcmpgtwLaneSplit, 0x65)
VITREO_VEX256_66_0F_PARITY(Vex256PcmpgtdLaneSplit, 0x66)
VITREO_VEX256_66_0F_PARITY(Vex256PavgbLaneSplit,   0xE0)
VITREO_VEX256_66_0F_PARITY(Vex256PavgwLaneSplit,   0xE3)
VITREO_VEX256_66_0F_PARITY(Vex256PminubLaneSplit,  0xDA)
VITREO_VEX256_66_0F_PARITY(Vex256PmaxubLaneSplit,  0xDE)
VITREO_VEX256_66_0F_PARITY(Vex256PminswLaneSplit,  0xEA)
VITREO_VEX256_66_0F_PARITY(Vex256PmaxswLaneSplit,  0xEE)
VITREO_VEX256_66_0F_PARITY(Vex256PandnLaneSplit,   0xDF)
VITREO_VEX256_66_0F_PARITY(Vex256AndpdLaneSplit,   0x54)
VITREO_VEX256_66_0F_PARITY(Vex256OrpdLaneSplit,    0x56)
VITREO_VEX256_66_0F_PARITY(Vex256XorpdLaneSplit,   0x57)
VITREO_VEX256_66_0F_PARITY(Vex256AndnpdLaneSplit,  0x55)

#undef VITREO_VEX256_66_0F_PARITY

// VEX.256.0F (pp=00) — the *PS bitwise aliases (C5 F4 op C2)
#define VITREO_VEX256_0F_PARITY(TestName, OpByte)                       \
    TEST_F(JitParity64Test, TestName) {                                 \
        check_native_parity({0xC5, 0xF4, (OpByte), 0xC2},               \
                            seed_vex256_operands,                       \
                            /*check_flags=*/false, /*check_xmm=*/true); \
    }

VITREO_VEX256_0F_PARITY(Vex256OrpsLaneSplit,   0x56)
VITREO_VEX256_0F_PARITY(Vex256XorpsLaneSplit,  0x57)
VITREO_VEX256_0F_PARITY(Vex256AndnpsLaneSplit, 0x55)

#undef VITREO_VEX256_0F_PARITY

// VEX.256.66.0F38 <op> /r — C4 E2 75 op C2
#define VITREO_VEX256_0F38_PARITY(TestName, OpByte)                     \
    TEST_F(JitParity64Test, TestName) {                                 \
        check_native_parity({0xC4, 0xE2, 0x75, (OpByte), 0xC2},         \
                            seed_vex256_operands,                       \
                            /*check_flags=*/false, /*check_xmm=*/true); \
    }

VITREO_VEX256_0F38_PARITY(Vex256PminsbLaneSplit,  0x38)
VITREO_VEX256_0F38_PARITY(Vex256PminsdLaneSplit,  0x39)
VITREO_VEX256_0F38_PARITY(Vex256PminuwLaneSplit,  0x3A)
VITREO_VEX256_0F38_PARITY(Vex256PminudLaneSplit,  0x3B)
VITREO_VEX256_0F38_PARITY(Vex256PmaxsbLaneSplit,  0x3C)
VITREO_VEX256_0F38_PARITY(Vex256PmaxsdLaneSplit,  0x3D)
VITREO_VEX256_0F38_PARITY(Vex256PmaxuwLaneSplit,  0x3E)
VITREO_VEX256_0F38_PARITY(Vex256PmaxudLaneSplit,  0x3F)
VITREO_VEX256_0F38_PARITY(Vex256PmulldLaneSplit,  0x40)
VITREO_VEX256_0F38_PARITY(Vex256PcmpeqqLaneSplit, 0x29)
VITREO_VEX256_0F38_PARITY(Vex256PcmpgtqLaneSplit, 0x37)

#undef VITREO_VEX256_0F38_PARITY

// The destination also naming SRC2 (`vpaddw ymm2, ymm1, ymm2`): the merge copy
// must not overwrite the second source before the lowering reads it.
TEST_F(JitParity64Test, Vex256PaddwDestEqualsSecondSource) {
    check_native_parity({0xC5, 0xF5, 0xFD, 0xD2}, seed_vex256_operands,
                        false, true);
}

// The destination also naming SRC1 (`vpaddw ymm1, ymm1, ymm2`).
TEST_F(JitParity64Test, Vex256PaddwDestEqualsFirstSource) {
    check_native_parity({0xC5, 0xF5, 0xFD, 0xCA}, seed_vex256_operands,
                        false, true);
}

// Memory second source: the high lane must read +16 bytes.
TEST_F(JitParity64Test, Vex256PaddwMemorySecondSource) {
    map_tracked_region(0x40000, 0x1000);
    check_native_parity(
        {0x48, 0xB8, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
         0xC5, 0xF5, 0xFD, 0x00},
        [this](CpuState& cpu) {
            seed_vex256_operands(cpu);
            for (int b = 0; b < 32; ++b) {
                (void)m_memory->write_u8(0x40000 + static_cast<GuestAddr>(b),
                                         static_cast<u8>(0x03 + b * 7));
            }
        },
        false, true);
}
