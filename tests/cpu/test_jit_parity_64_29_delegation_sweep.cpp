// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests — 64-bit long mode (CPU-048): forms that used
// to delegate (2026-09-26 sweep).

#include "jit_parity_64_fixture.hpp"

// ============================================================================
// Satisfactory (UE5) delegation sweep, 2026-09-26.
//
// Every form below used to delegate to the interpreter. Each test runs the
// JIT with VITREO_JIT_NO_DELEGATE=1 (check_native_parity) so a regression
// back to the delegate fails outright rather than passing on the
// interpreter's answer. Absolute expectations, where given, are taken from
// the Intel SDM Vol. 2 operation text, not from running either engine.
// ============================================================================
namespace {

// VEX3 encoder: map 1=0F 2=0F38 3=0F3A; pp 0=none 1=66 2=F3 3=F2.
// `mem` selects [rax] (ModRM mod=00 rm=000) as the r/m operand.
std::vector<u8> vex3(u8 map, u8 pp, bool l, bool w, u8 vvvv, u8 op, u8 reg,
                     u8 rm, bool mem, std::initializer_list<u8> tail = {}) {
    std::vector<u8> v;
    v.push_back(0xC4);
    v.push_back(static_cast<u8>((reg < 8 ? 0x80 : 0) | 0x40 |
                                ((mem || rm < 8) ? 0x20 : 0) | map));
    v.push_back(static_cast<u8>((w ? 0x80 : 0) | ((~vvvv & 15) << 3) |
                                (l ? 4 : 0) | pp));
    v.push_back(op);
    v.push_back(mem ? static_cast<u8>((reg & 7) << 3)
                    : static_cast<u8>(0xC0 | ((reg & 7) << 3) | (rm & 7)));
    for (u8 b : tail) v.push_back(b);
    return v;
}

constexpr GuestAddr kSatData = 0x8000;

// Finite, exception-free packed values in every register half, plus the same
// shape in memory at kSatData (+3 when `unaligned`), so both lanes of every
// operand carry distinct data.
void seed_fp(CpuState& cpu, bool dbl) {
    for (int r = 0; r < 16; ++r) {
        for (int i = 0; i < 4; ++i) {
            if (dbl) {
                if (i < 2) {
                    cpu.sse.xmm[r].f64_[i] = 1.25 * (r + 1) - 0.75 * i;
                    cpu.avx.ymm_high[r].f64_[i] = -3.5 * (r + 2) + 0.5 * i;
                }
            } else {
                cpu.sse.xmm[r].f32_[i] = 1.5f * static_cast<float>(r + 1) - 0.25f * static_cast<float>(i);
                cpu.avx.ymm_high[r].f32_[i] = -2.75f * static_cast<float>(r + 2) + 0.5f * static_cast<float>(i);
            }
        }
    }
    cpu.gpr.rax = kSatData;
    cpu.sse.mxcsr = 0x1F80;
}

}  // namespace

// ---- VEX.256 lane-split additions: register, [rax], aliased destination ----
TEST_F(JitParity64Test, SatisfactoryVex256PackedDoubleArithmeticNative) {
    double mem[4] = {0.5, -1.25, 3.0, 7.5};
    ASSERT_TRUE(m_memory->write_bytes(kSatData, reinterpret_cast<u8*>(mem), sizeof(mem)));
    // 66 0F: ADDPD 58, MULPD 59, SUBPD 5C, MINPD 5D, DIVPD 5E, MAXPD 5F.
    for (u8 op : {u8{0x58}, u8{0x59}, u8{0x5C}, u8{0x5D}, u8{0x5E}, u8{0x5F}}) {
        for (bool mem_form : {false, true}) {
            for (u8 dst : {u8{0}, u8{2}, u8{9}}) {
                // dst==src1 (2 with vvvv=2), dst==r/m (2 with rm=2) are covered.
                m_jit = std::make_unique<JitCompiler>(*m_memory);
                SCOPED_TRACE(::testing::Message() << "op=" << std::hex << +op
                             << " mem=" << mem_form << " dst=" << +dst);
                auto code = vex3(1, 1, true, false, 2, op, dst, 2, mem_form);
                check_parity(code.data(), code.size(),
                             [](CpuState& c) { seed_fp(c, true); },
                             false, true, true);
                ScopedEnvironmentVariable nd("VITREO_JIT_NO_DELEGATE", "1");
                m_jit = std::make_unique<JitCompiler>(*m_memory);
                check_parity(code.data(), code.size(),
                             [](CpuState& c) { seed_fp(c, true); },
                             false, true, true);
            }
        }
    }
}

TEST_F(JitParity64Test, SatisfactoryVex256PackedDoubleNaNAndInfinityMatch) {
    // The FINITE_FP cold arm (Inf/NaN result) and the MINMAX NaN rule must
    // hold in the high lane too, where the helper runs on swapped halves.
    for (u8 op : {u8{0x58}, u8{0x59}, u8{0x5C}, u8{0x5D}, u8{0x5F}}) {
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        SCOPED_TRACE(::testing::Message() << "op=" << std::hex << +op);
        auto code = vex3(1, 1, true, false, 1, op, 0, 2, false);
        // Observe lazy host FP flags through the actual guest contract.
        // Starting with LDMXCSR also makes this independent of host flags
        // left by earlier cases; STMXCSR must expose NaN IE on both lanes.
        ASSERT_TRUE(m_memory->write_u32(0x2040, 0x1f80));
        code.insert(code.begin(), {0x0f,0xae,0x13});
        code.insert(code.end(), {0x0f,0xae,0x1b});
        ScopedEnvironmentVariable nd("VITREO_JIT_NO_DELEGATE", "1");
        check_parity(code.data(), code.size(), [](CpuState& c) {
            seed_fp(c, true);
            c.gpr.rbx = 0x2040;
            const double inf = std::numeric_limits<double>::infinity();
            c.avx.ymm_high[1].f64_[0] = inf;
            c.avx.ymm_high[2].f64_[0] = -inf;       // inf - inf / inf + -inf
            c.avx.ymm_high[1].f64_[1] = std::numeric_limits<double>::quiet_NaN();
            c.sse.xmm[2].f64_[1] = std::numeric_limits<double>::quiet_NaN();
        }, false, true, true);
    }
}

TEST_F(JitParity64Test, SatisfactoryVex256CmppdAllPredicatesNative) {
    ScopedEnvironmentVariable nd("VITREO_JIT_NO_DELEGATE", "1");
    for (u32 pred = 0; pred < 32; ++pred) {
        for (bool mem_form : {false, true}) {
            m_jit = std::make_unique<JitCompiler>(*m_memory);
            auto code = vex3(1, 1, true, false, 1, 0xC2, 0, 2, mem_form,
                             {static_cast<u8>(pred)});
            check_parity(code.data(), code.size(), [](CpuState& c) {
                seed_fp(c, true);
                c.avx.ymm_high[1].f64_[1] = std::numeric_limits<double>::quiet_NaN();
                c.sse.xmm[2].f64_[0] = c.sse.xmm[1].f64_[0];
            }, false, true, true);
        }
    }
}

TEST_F(JitParity64Test, SatisfactoryVex256ShufpsShufpdEveryImmediateNative) {
    ScopedEnvironmentVariable nd("VITREO_JIT_NO_DELEGATE", "1");
    float mem[8] = {10, 11, 12, 13, 14, 15, 16, 17};
    ASSERT_TRUE(m_memory->write_bytes(kSatData, reinterpret_cast<u8*>(mem), sizeof(mem)));
    for (u8 pp : {u8{0}, u8{1}}) {                // VSHUFPS, VSHUFPD
        for (u32 imm = 0; imm < 256; imm += (pp == 1 ? 1 : 7)) {
            for (bool mem_form : {false, true}) {
                m_jit = std::make_unique<JitCompiler>(*m_memory);
                SCOPED_TRACE(::testing::Message() << "pp=" << +pp << " imm="
                             << imm << " mem=" << mem_form);
                auto code = vex3(1, pp, true, false, 1, 0xC6, 0, 2, mem_form,
                                 {static_cast<u8>(imm)});
                check_parity(code.data(), code.size(),
                             [](CpuState& c) { seed_fp(c, false); },
                             false, true, true);
            }
        }
    }
}

TEST_F(JitParity64Test, SatisfactoryVex256ShufpsHighLaneMatchesSdm) {
    // Intel SDM Vol. 2 VSHUFPS (VEX.256): DEST[159:128] := Select4(SRC1[255:128],
    // imm8[1:0]) ... DEST[255:224] := Select4(SRC2[255:128], imm8[7:6]).
    // VSHUFPD (VEX.256): DEST[191:128] := imm8[2] ? SRC1[255:192] : SRC1[191:128];
    //                    DEST[255:192] := imm8[3] ? SRC2[255:192] : SRC2[191:128].
    ScopedEnvironmentVariable nd("VITREO_JIT_NO_DELEGATE", "1");
    auto code = vex3(1, 0, true, false, 1, 0xC6, 0, 2, false, {0x1B});
    check_parity(code.data(), code.size(), [](CpuState& c) {
        for (int i = 0; i < 4; ++i) {
            c.sse.xmm[1].u32_[i] = 0x100 + i; c.avx.ymm_high[1].u32_[i] = 0x140 + i;
            c.sse.xmm[2].u32_[i] = 0x200 + i; c.avx.ymm_high[2].u32_[i] = 0x240 + i;
        }
    }, false, true, true);
    // imm 0x1B = 00 01 10 11: lanes pick 3,2 from SRC1 and 1,0 from SRC2.
    const u32 want_hi[4] = {0x143, 0x142, 0x241, 0x240};
    for (int i = 0; i < 4; ++i) EXPECT_EQ(m_last_jit_cpu.avx.ymm_high[0].u32_[i], want_hi[i]);

    m_jit = std::make_unique<JitCompiler>(*m_memory);
    auto pd = vex3(1, 1, true, false, 1, 0xC6, 0, 2, false, {0x06});  // 0110b
    check_parity(pd.data(), pd.size(), [](CpuState& c) {
        for (int i = 0; i < 2; ++i) {
            c.sse.xmm[1].u64_[i] = 0x10 + i; c.avx.ymm_high[1].u64_[i] = 0x14 + i;
            c.sse.xmm[2].u64_[i] = 0x20 + i; c.avx.ymm_high[2].u64_[i] = 0x24 + i;
        }
    }, false, true, true);
    EXPECT_EQ(m_last_jit_cpu.sse.xmm[0].u64_[0], 0x10u);        // imm[0]=0
    EXPECT_EQ(m_last_jit_cpu.sse.xmm[0].u64_[1], 0x21u);        // imm[1]=1
    EXPECT_EQ(m_last_jit_cpu.avx.ymm_high[0].u64_[0], 0x15u);   // imm[2]=1
    EXPECT_EQ(m_last_jit_cpu.avx.ymm_high[0].u64_[1], 0x24u);   // imm[3]=0
}

TEST_F(JitParity64Test, SatisfactoryAddsubAllWidthsNative) {
    ScopedEnvironmentVariable nd("VITREO_JIT_NO_DELEGATE", "1");
    float mem[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    ASSERT_TRUE(m_memory->write_bytes(kSatData, reinterpret_cast<u8*>(mem), sizeof(mem)));
    for (u8 pp : {u8{3}, u8{1}}) {                // VADDSUBPS, VADDSUBPD
        for (bool l : {false, true}) {
            for (bool mem_form : {false, true}) {
                for (u8 dst : {u8{0}, u8{1}, u8{2}}) {
                    m_jit = std::make_unique<JitCompiler>(*m_memory);
                    SCOPED_TRACE(::testing::Message() << "pp=" << +pp << " l=" << l
                                 << " mem=" << mem_form << " dst=" << +dst);
                    auto code = vex3(1, pp, l, false, 1, 0xD0, dst, 2, mem_form);
                    check_parity(code.data(), code.size(),
                                 [pp](CpuState& c) { seed_fp(c, pp == 1); },
                                 false, true, true);
                }
            }
        }
    }
}

TEST_F(JitParity64Test, SatisfactoryAddsubpsNaNRulesMatchSdm) {
    // Intel SDM Vol. 1 §4.8.3.4 Table 4-7 (exceptions masked): SRC1 NaN wins
    // (quietened), else SRC2 NaN, else an invalid op returns the QNaN
    // floating-point indefinite 0xFFC00000. Legacy F2 0F D0 C1 = addsubps xmm0, xmm1.
    ScopedEnvironmentVariable nd("VITREO_JIT_NO_DELEGATE", "1");
    check_parity({0xF2, 0x0F, 0xD0, 0xC1}, [](CpuState& c) {
        const float inf = std::numeric_limits<float>::infinity();
        c.sse.xmm[0].f32_[0] = inf;        c.sse.xmm[1].f32_[0] = inf;   // inf-inf
        c.sse.xmm[0].u32_[1] = 0x7FC00001; c.sse.xmm[1].u32_[1] = 0x7F800002; // Q + S
        c.sse.xmm[0].f32_[2] = 1.0f;       c.sse.xmm[1].u32_[2] = 0xFF800003; // S in src2
        c.sse.xmm[0].f32_[3] = 2.0f;       c.sse.xmm[1].f32_[3] = 0.5f;
    }, false, true, true);
    EXPECT_EQ(m_last_jit_cpu.sse.xmm[0].u32_[0], 0xFFC00000u);
    EXPECT_EQ(m_last_jit_cpu.sse.xmm[0].u32_[1], 0x7FC00001u);
    EXPECT_EQ(m_last_jit_cpu.sse.xmm[0].u32_[2], 0xFFC00003u);
    EXPECT_EQ(m_last_jit_cpu.sse.xmm[0].f32_[3], 2.5f);
    EXPECT_EQ(m_last_interp_cpu.sse.xmm[0].u32_[0], 0xFFC00000u);
    EXPECT_EQ(m_last_interp_cpu.sse.xmm[0].u32_[2], 0xFFC00003u);
}

TEST_F(JitParity64Test, SatisfactoryVex256RcpRsqrtNative) {
    ScopedEnvironmentVariable nd("VITREO_JIT_NO_DELEGATE", "1");
    float mem[8] = {1, 2, 4, 8, 0.5f, 16, 3, 9};
    ASSERT_TRUE(m_memory->write_bytes(kSatData, reinterpret_cast<u8*>(mem), sizeof(mem)));
    for (u8 op : {u8{0x53}, u8{0x52}}) {
        for (bool l : {false, true}) {
            for (bool mem_form : {false, true}) {
                m_jit = std::make_unique<JitCompiler>(*m_memory);
                auto code = vex3(1, 0, l, false, 0, op, 3, 2, mem_form);
                check_parity(code.data(), code.size(), [](CpuState& c) {
                    seed_fp(c, false);
                    for (int i = 0; i < 4; ++i) {
                        c.sse.xmm[2].f32_[i] = 0.25f + static_cast<float>(i);
                        c.avx.ymm_high[2].f32_[i] = 4.0f + static_cast<float>(i);
                    }
                }, false, true, true);
            }
        }
    }
}

// ---- FMA3 alternating: VFMADDSUB / VFMSUBADD, all orders and widths -------
TEST_F(JitParity64Test, SatisfactoryFmaddsubFmsubaddEveryFormNative) {
    ScopedEnvironmentVariable nd("VITREO_JIT_NO_DELEGATE", "1");
    float memf[8] = {0.5f, -1.5f, 2.0f, 3.25f, -4.0f, 5.5f, 6.0f, -7.25f};
    ASSERT_TRUE(m_memory->write_bytes(kSatData, reinterpret_cast<u8*>(memf), sizeof(memf)));
    const u8 ops[] = {0x96, 0xA6, 0xB6, 0x97, 0xA7, 0xB7};
    for (u8 op : ops) {
        for (bool w : {false, true}) {
            for (bool l : {false, true}) {
                for (bool mem_form : {false, true}) {
                    for (u8 dst : {u8{0}, u8{1}, u8{8}}) {
                        m_jit = std::make_unique<JitCompiler>(*m_memory);
                        SCOPED_TRACE(::testing::Message() << "op=" << std::hex << +op
                                     << " w=" << w << " l=" << l << " mem=" << mem_form
                                     << " dst=" << +dst);
                        auto code = vex3(2, 1, l, w, 1, op, dst, 2, mem_form);
                        check_parity(code.data(), code.size(),
                                     [w](CpuState& c) { seed_fp(c, w); },
                                     false, true, true);
                    }
                }
            }
        }
    }
}

TEST_F(JitParity64Test, SatisfactoryFmaddsubNaNOperandsMatch) {
    // QNaN operands in each role, both lanes. (inf*0 is avoided: the
    // interpreter records MXCSR.IE from the host FPU and the JIT's FMA
    // lowerings do not model sticky bits -- a known shared gap.)
    ScopedEnvironmentVariable nd("VITREO_JIT_NO_DELEGATE", "1");
    for (u8 op : {u8{0x96}, u8{0xB7}}) {
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        auto code = vex3(2, 1, true, false, 1, op, 0, 2, false);
        check_parity(code.data(), code.size(), [](CpuState& c) {
            seed_fp(c, false);
            c.sse.xmm[0].u32_[0] = 0x7FC00011;
            c.sse.xmm[1].u32_[1] = 0xFFC00022;
            c.sse.xmm[2].u32_[2] = 0x7FC00033;
            c.avx.ymm_high[0].u32_[3] = 0x7FC00044;
            c.avx.ymm_high[1].u32_[3] = 0xFFC00055;
            c.avx.ymm_high[2].u32_[0] = 0x7FC00066;
        }, false, true, true);
    }
}

TEST_F(JitParity64Test, SatisfactoryFmaddsubAlternationMatchesSdm) {
    // Intel SDM Vol. 2 VFMADDSUB231PS: even elements DEST := SRC2*SRC3 - DEST,
    // odd elements DEST := SRC2*SRC3 + DEST, across all eight (VEX.256).
    ScopedEnvironmentVariable nd("VITREO_JIT_NO_DELEGATE", "1");
    auto code = vex3(2, 1, true, false, 1, 0xB6, 0, 2, false);
    check_parity(code.data(), code.size(), [](CpuState& c) {
        for (int i = 0; i < 4; ++i) {
            c.sse.xmm[0].f32_[i] = 1.0f; c.avx.ymm_high[0].f32_[i] = 1.0f;
            c.sse.xmm[1].f32_[i] = 2.0f; c.avx.ymm_high[1].f32_[i] = 3.0f;
            c.sse.xmm[2].f32_[i] = 5.0f; c.avx.ymm_high[2].f32_[i] = 7.0f;
        }
    }, false, true, true);
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(m_last_jit_cpu.sse.xmm[0].f32_[i], (i & 1) ? 11.0f : 9.0f);
        EXPECT_EQ(m_last_jit_cpu.avx.ymm_high[0].f32_[i], (i & 1) ? 22.0f : 20.0f);
    }
}

// ---- F16C VCVTPH2PS memory forms -------------------------------------------
TEST_F(JitParity64Test, SatisfactoryVcvtph2psMemoryMatchesSdm) {
    // Intel SDM Vol. 2 VCVTPH2PS: exact half->single widening. Values:
    // 1.0, -2.0, +inf, smallest subnormal 2^-24, SNaN 0x7D00 (quietened:
    // 0x7FE00000), -0, 65504 (max half), 0.5.
    const u16 halves[8] = {0x3C00, 0xC000, 0x7C00, 0x0001,
                           0x7D00, 0x8000, 0x7BFF, 0x3800};
    const u32 want[8] = {0x3F800000, 0xC0000000, 0x7F800000, 0x33800000,
                         0x7FE00000, 0x80000000, 0x477FE000, 0x3F000000};
    ScopedEnvironmentVariable nd("VITREO_JIT_NO_DELEGATE", "1");
    for (GuestAddr base : {GuestAddr{kSatData}, GuestAddr{kSatData + 3}}) {
        ASSERT_TRUE(m_memory->write_bytes(base, reinterpret_cast<const u8*>(halves),
                                          sizeof(halves)));
        for (bool l : {false, true}) {
            for (u8 dst : {u8{0}, u8{9}}) {
                m_jit = std::make_unique<JitCompiler>(*m_memory);
                SCOPED_TRACE(::testing::Message() << "base=" << std::hex << base
                             << " l=" << l << " dst=" << +dst);
                auto code = vex3(2, 1, l, false, 0, 0x13, dst, 0, true);
                check_parity(code.data(), code.size(), [base](CpuState& c) {
                    c.gpr.rax = base;
                    for (int i = 0; i < 4; ++i) {
                        c.sse.xmm[0].u32_[i] = c.sse.xmm[9].u32_[i] = 0xDEADBEEF;
                        c.avx.ymm_high[0].u32_[i] = c.avx.ymm_high[9].u32_[i] = ~0u;
                    }
                }, false, true, true);
                const auto& out = m_last_jit_cpu;
                for (int i = 0; i < 4; ++i) {
                    EXPECT_EQ(out.sse.xmm[dst].u32_[i], want[i]) << i;
                    EXPECT_EQ(out.avx.ymm_high[dst].u32_[i], l ? want[4 + i] : 0u) << i;
                }
            }
        }
    }
}

// ---- AVX memory forms that had no JIT path --------------------------------
TEST_F(JitParity64Test, SatisfactoryVinsertf128AndVpermpdMemoryNative) {
    ScopedEnvironmentVariable nd("VITREO_JIT_NO_DELEGATE", "1");
    u64 mem[4] = {0x1111111111111111ull, 0x2222222222222222ull,
                  0x3333333333333333ull, 0x4444444444444444ull};
    for (GuestAddr base : {GuestAddr{kSatData}, GuestAddr{kSatData + 5}}) {
        ASSERT_TRUE(m_memory->write_bytes(base, reinterpret_cast<u8*>(mem), sizeof(mem)));
        auto seed = [base](CpuState& c) {
            for (int r = 0; r < 16; ++r) {
                c.sse.xmm[r].u64_[0] = 0xA0 + r; c.sse.xmm[r].u64_[1] = 0xB0 + r;
                c.avx.ymm_high[r].u64_[0] = 0xC0 + r; c.avx.ymm_high[r].u64_[1] = 0xD0 + r;
            }
            c.gpr.rax = base;
        };
        // VINSERTF128 (18) / VINSERTI128 (38) ymm1, ymm2, m128, imm; dst==src1 too.
        for (u8 op : {u8{0x18}, u8{0x38}}) {
            for (u8 imm : {u8{0}, u8{1}, u8{0xFE}}) {
                for (u8 src1 : {u8{2}, u8{1}}) {
                    m_jit = std::make_unique<JitCompiler>(*m_memory);
                    auto code = vex3(3, 1, true, false, src1, op, 1, 0, true, {imm});
                    check_parity(code.data(), code.size(), seed, false, true, true);
                    // Intel SDM: DEST := SRC1; DEST[128*imm8[0] +: 128] := SRC2.
                    const bool hi = imm & 1;
                    const auto& o = m_last_jit_cpu;
                    EXPECT_EQ((hi ? o.avx.ymm_high[1] : o.sse.xmm[1]).u64_[0], mem[0]);
                    EXPECT_EQ((hi ? o.sse.xmm[1] : o.avx.ymm_high[1]).u64_[0],
                              hi ? 0xA0u + src1 : 0xC0u + src1);
                }
            }
        }
        // VPERMQ (00) / VPERMPD (01) ymm1, m256, imm (VEX.256.66.0F3A.W1).
        for (u8 op : {u8{0x00}, u8{0x01}}) {
            for (u8 imm : {u8{0x1B}, u8{0x00}, u8{0xE4}, u8{0x4E}}) {
                m_jit = std::make_unique<JitCompiler>(*m_memory);
                auto code = vex3(3, 1, true, true, 0, op, 1, 0, true, {imm});
                check_parity(code.data(), code.size(), seed, false, true, true);
                const auto& o = m_last_jit_cpu;
                const u64 got[4] = {o.sse.xmm[1].u64_[0], o.sse.xmm[1].u64_[1],
                                    o.avx.ymm_high[1].u64_[0], o.avx.ymm_high[1].u64_[1]};
                for (int i = 0; i < 4; ++i) EXPECT_EQ(got[i], mem[(imm >> (2 * i)) & 3]);
            }
        }
    }
}

TEST_F(JitParity64Test, SatisfactoryCmppsCmppdMemoryIn64BitBlocksNative) {
    ScopedEnvironmentVariable nd("VITREO_JIT_NO_DELEGATE", "1");
    float mem[8] = {1.0f, 2.0f, -3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
    ASSERT_TRUE(m_memory->write_bytes(kSatData, reinterpret_cast<u8*>(mem), sizeof(mem)));
    for (u8 pred = 0; pred < 8; ++pred) {
        // legacy cmpps xmm1, [rax], pred (0F C2 08 ib) and cmppd (66 ...).
        check_parity({0x0F, 0xC2, 0x08, pred},
                     [](CpuState& c) { seed_fp(c, false); c.sse.xmm[1].f32_[0] = 1.0f; },
                     false, true, true);
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        check_parity({0x66, 0x0F, 0xC2, 0x08, pred},
                     [](CpuState& c) { seed_fp(c, true); }, false, true, true);
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        auto v = vex3(1, 0, false, false, 3, 0xC2, 1, 0, true, {pred});
        check_parity(v.data(), v.size(), [](CpuState& c) { seed_fp(c, false); },
                     false, true, true);
        m_jit = std::make_unique<JitCompiler>(*m_memory);
    }
}

TEST_F(JitParity64Test, SatisfactoryCvtpd2psMemoryAndVex256Native) {
    ScopedEnvironmentVariable nd("VITREO_JIT_NO_DELEGATE", "1");
    double mem[4] = {1.0, 0.1, -1e300, 3.5};   // 0.1 inexact, -1e300 overflows
    ASSERT_TRUE(m_memory->write_bytes(kSatData, reinterpret_cast<u8*>(mem), sizeof(mem)));
    // legacy cvtpd2ps xmm1, [rax]
    check_parity({0x66, 0x0F, 0x5A, 0x08},
                 [](CpuState& c) { seed_fp(c, true); }, false, true, true);
    // VEX.256 vcvtpd2ps xmm1, ymm2 / xmm1, m256 / xmm2, ymm2 (dst == src).
    for (bool mem_form : {false, true}) {
        for (u8 dst : {u8{1}, u8{2}}) {
            m_jit = std::make_unique<JitCompiler>(*m_memory);
            auto code = vex3(1, 1, true, false, 0, 0x5A, dst, 2, mem_form);
            check_parity(code.data(), code.size(),
                         [](CpuState& c) { seed_fp(c, true); }, false, true, true);
            // Intel SDM Vol. 2 VCVTPD2PS (VEX.256): DEST[127:0] = four
            // converted singles, DEST[MAXVL-1:128] = 0.
            EXPECT_EQ(m_last_jit_cpu.avx.ymm_high[dst].u64_[0], 0u);
            EXPECT_EQ(m_last_jit_cpu.avx.ymm_high[dst].u64_[1], 0u);
            if (mem_form) {
                EXPECT_EQ(m_last_jit_cpu.sse.xmm[dst].f32_[0], 1.0f);
                EXPECT_EQ(m_last_jit_cpu.sse.xmm[dst].f32_[3], 3.5f);
            }
        }
    }
}

TEST_F(JitParity64Test, SatisfactoryPinsrqMemoryNative) {
    ScopedEnvironmentVariable nd("VITREO_JIT_NO_DELEGATE", "1");
    ASSERT_TRUE(m_memory->write_u64(kSatData + 3, 0x0123456789ABCDEFull).is_ok());
    for (u8 lane : {u8{0}, u8{1}}) {
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        check_parity({0x66, 0x48, 0x0F, 0x3A, 0x22, 0x08, lane},
                     [](CpuState& c) { seed_fp(c, true); c.gpr.rax = kSatData + 3; },
                     false, true, true);
        EXPECT_EQ(m_last_jit_cpu.sse.xmm[1].u64_[lane], 0x0123456789ABCDEFull);
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        auto v = vex3(3, 1, false, true, 2, 0x22, 1, 0, true, {lane});
        check_parity(v.data(), v.size(),
                     [](CpuState& c) { seed_fp(c, true); c.gpr.rax = kSatData + 3; },
                     false, true, true);
        EXPECT_EQ(m_last_jit_cpu.avx.ymm_high[1].u64_[0], 0u);
    }
}

// A faulting read anywhere in a VEX.256 memory operand leaves DEST untouched
// (hardware never commits a faulting instruction). The lane split used to
// commit lane 0 before lane 1's read faulted; both engines must now agree
// with the architecture.
TEST_F(JitParity64Test, SatisfactoryVex256MemoryFaultPreservesDestination) {
    ScopedEnvironmentVariable nd("VITREO_JIT_NO_DELEGATE", "1");
    struct Form { const char* name; std::vector<u8> code; u32 bytes; };
    const std::vector<Form> forms = {
        {"vshufps", vex3(1, 0, true, false, 2, 0xC6, 1, 0, true, {0x44}), 32},
        {"vmulpd", vex3(1, 1, true, false, 2, 0x59, 1, 0, true), 32},
        {"vaddsubps", vex3(1, 3, true, false, 2, 0xD0, 1, 0, true), 32},
        {"vcmppd", vex3(1, 1, true, false, 2, 0xC2, 1, 0, true, {1}), 32},
        {"vrcpps", vex3(1, 0, true, false, 0, 0x53, 1, 0, true), 32},
        {"vfmaddsub231ps", vex3(2, 1, true, false, 2, 0xB6, 1, 0, true), 32},
        {"vcvtpd2ps", vex3(1, 1, true, false, 0, 0x5A, 1, 0, true), 32},
        {"vpermpd", vex3(3, 1, true, true, 0, 0x01, 1, 0, true, {0x1B}), 32},
        {"vcvtph2ps", vex3(2, 1, true, false, 0, 0x13, 1, 0, true), 16},
        {"vinsertf128", vex3(3, 1, true, false, 2, 0x18, 1, 0, true, {1}), 16},
    };
    for (const Form& f : forms) {
        ASSERT_TRUE(m_memory->write_bytes(0x1000, f.code.data(), f.code.size()));
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        BasicBlock block = build_block(f.code.data(), f.code.size());
        CompiledBlock* compiled = m_jit->compile(block);
        ASSERT_NE(compiled, nullptr) << f.name;
        for (u32 readable = 0; readable < f.bytes; readable += 8) {
            SCOPED_TRACE(::testing::Message() << f.name << " readable=" << readable);
            CpuState cpu;
            init_cpu(cpu);
            cpu.gpr.rax = 0x11000 - readable;   // the mapping ends at 0x11000
            for (int i = 0; i < 2; ++i) {
                cpu.sse.xmm[1].u64_[i] = 0x1122334455667788ull;
                cpu.avx.ymm_high[1].u64_[i] = 0x8877665544332211ull;
            }
            CpuState interpreted = cpu;
            Interpreter interpreter(interpreted, *m_memory);
            EXPECT_EQ(interpreter.step(), ExecResult::Exception);
            EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
            ASSERT_TRUE(cpu.pending_raise.active);
            EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
            for (int i = 0; i < 2; ++i) {
                EXPECT_EQ(interpreted.sse.xmm[1].u64_[i], 0x1122334455667788ull);
                EXPECT_EQ(interpreted.avx.ymm_high[1].u64_[i], 0x8877665544332211ull);
                EXPECT_EQ(cpu.sse.xmm[1].u64_[i], 0x1122334455667788ull);
                EXPECT_EQ(cpu.avx.ymm_high[1].u64_[i], 0x8877665544332211ull);
            }
        }
    }
}

// ---- LOCK r/m8 and r/m16: every form, flags included -----------------------
TEST_F(JitParity64Test, SatisfactoryLockByteAndWordFormsNative) {
    ScopedEnvironmentVariable nd("VITREO_JIT_NO_DELEGATE", "1");
    m_compare_memory = true;
    struct Form { const char* name; std::vector<u8> code; };
    const std::vector<Form> forms = {
        {"add r/m8,r8", {0xF0, 0x00, 0x08}}, {"or r/m8,r8", {0xF0, 0x08, 0x08}},
        {"adc r/m8,r8", {0xF0, 0x10, 0x08}}, {"sbb r/m8,r8", {0xF0, 0x18, 0x08}},
        {"and r/m8,r8", {0xF0, 0x20, 0x08}}, {"sub r/m8,r8", {0xF0, 0x28, 0x08}},
        {"xor r/m8,r8", {0xF0, 0x30, 0x08}},
        {"add r/m8,imm8", {0xF0, 0x80, 0x00, 0x81}}, {"or r/m8,imm8", {0xF0, 0x80, 0x08, 0x81}},
        {"adc r/m8,imm8", {0xF0, 0x80, 0x10, 0x7F}}, {"sbb r/m8,imm8", {0xF0, 0x80, 0x18, 0x7F}},
        {"and r/m8,imm8", {0xF0, 0x80, 0x20, 0x81}}, {"sub r/m8,imm8", {0xF0, 0x80, 0x28, 0x81}},
        {"xor r/m8,imm8", {0xF0, 0x80, 0x30, 0x81}},
        {"inc r/m8", {0xF0, 0xFE, 0x00}}, {"dec r/m8", {0xF0, 0xFE, 0x08}},
        {"not r/m8", {0xF0, 0xF6, 0x10}}, {"neg r/m8", {0xF0, 0xF6, 0x18}},
        {"xadd r/m8,cl", {0xF0, 0x0F, 0xC0, 0x08}},
        {"xadd r/m8,ah", {0xF0, 0x0F, 0xC0, 0x20}},
        {"xadd r/m8,spl", {0xF0, 0x40, 0x0F, 0xC0, 0x20}},
        {"xadd r/m8,r9b", {0xF0, 0x44, 0x0F, 0xC0, 0x08}},
        {"xchg r/m8,ch", {0xF0, 0x86, 0x28}},
        {"xchg r/m8,cl (no lock)", {0x86, 0x08}},
        {"add r/m16,r16", {0xF0, 0x66, 0x01, 0x08}},
        {"adc r/m16,r16", {0xF0, 0x66, 0x11, 0x08}},
        {"sbb r/m16,r16", {0xF0, 0x66, 0x19, 0x08}},
        {"sub r/m16,r16", {0xF0, 0x66, 0x29, 0x08}},
        {"xor r/m16,r16", {0xF0, 0x66, 0x31, 0x08}},
        {"add r/m16,imm16", {0xF0, 0x66, 0x81, 0x00, 0x01, 0x80}},
        {"sub r/m16,imm8", {0xF0, 0x66, 0x83, 0x28, 0xFF}},
        {"adc r/m16,imm8", {0xF0, 0x66, 0x83, 0x10, 0x01}},
        {"sbb r/m16,imm16", {0xF0, 0x66, 0x81, 0x18, 0x34, 0x12}},
        {"xor r/m16,imm8", {0xF0, 0x66, 0x83, 0x30, 0x80}},
        {"inc r/m16", {0xF0, 0x66, 0xFF, 0x00}}, {"dec r/m16", {0xF0, 0x66, 0xFF, 0x08}},
        {"not r/m16", {0xF0, 0x66, 0xF7, 0x10}}, {"neg r/m16", {0xF0, 0x66, 0xF7, 0x18}},
        {"xadd r/m16,r16", {0xF0, 0x66, 0x0F, 0xC1, 0x08}},
        {"xchg r/m16,r16", {0xF0, 0x66, 0x87, 0x08}},
        {"cmpxchg r/m16,r16", {0xF0, 0x66, 0x0F, 0xB1, 0x08}},
    };
    const u16 mem_values[] = {0x0000, 0x007F, 0x0080, 0x00FF, 0x7FFF, 0x8000, 0xFFFF, 0x1234};
    const u64 reg_values[] = {0x01, 0x80, 0xFF, 0x7FFF, 0x10000000000FF00ull};
    for (const Form& f : forms) {
        for (GuestAddr addr : {GuestAddr{0x3000}, GuestAddr{0x3001}}) {
            for (u16 m : mem_values) {
                for (u64 r : reg_values) {
                    for (bool cf : {false, true}) {
                        SCOPED_TRACE(::testing::Message() << f.name << " addr=" << std::hex
                                     << addr << " mem=" << m << " reg=" << r << " cf=" << cf);
                        ASSERT_TRUE(m_memory->write_u16(addr, m).is_ok());
                        m_jit = std::make_unique<JitCompiler>(*m_memory);
                        check_parity(f.code.data(), f.code.size(), [&](CpuState& c) {
                            c.gpr.rax = (addr & ~u64{0xFF}) | (addr & 0xFF);
                            c.gpr.set64(RCX, r);
                            c.gpr.set64(RSP, 0x10000);
                            c.gpr.set64(R9, r ^ 0x5A);
                            c.flags.rflags = 0x202 | (cf ? 1u : 0u);
                        }, /*check_flags=*/true, /*check_xmm=*/false,
                           /*require_jit=*/true, /*check_af=*/true);
                        ASSERT_TRUE(m_last_parity_ok);
                    }
                }
            }
        }
    }
}

TEST_F(JitParity64Test, SatisfactoryLockXaddByteMatchesSdm) {
    // Intel SDM Vol. 2 XADD: TEMP := SRC + DEST; SRC := DEST; DEST := TEMP.
    // 0x7F + 0x01 = 0x80: OF=1, SF=1, CF=0, ZF=0, AF=1.
    ScopedEnvironmentVariable nd("VITREO_JIT_NO_DELEGATE", "1");
    ASSERT_TRUE(m_memory->write_u8(0x3000, 0x7F).is_ok());
    check_parity({0xF0, 0x0F, 0xC0, 0x08}, [](CpuState& c) {
        c.gpr.rax = 0x3000;
        c.gpr.rcx = 0xAAAA'BBBB'CCCC'DD01ull;
    }, true, false, true, true);
    EXPECT_EQ(m_memory->read_u8(0x3000).value_or(0), 0x80u);
    EXPECT_EQ(m_last_jit_cpu.gpr.rcx, 0xAAAA'BBBB'CCCC'DD7Full);
    EXPECT_TRUE(m_last_jit_cpu.flags.of());
    EXPECT_TRUE(m_last_jit_cpu.flags.sf());
    EXPECT_FALSE(m_last_jit_cpu.flags.cf());
    EXPECT_FALSE(m_last_jit_cpu.flags.zf());
    EXPECT_TRUE(m_last_jit_cpu.flags.af());
}

TEST_F(JitParity64Test, SatisfactoryLockByteFaultLeavesRegistersAndFlags) {
    // An unmapped LOCK destination raises #PF (STATUS_ACCESS_VIOLATION) and
    // commits nothing: XADD's register and the flags keep their old values.
    ScopedEnvironmentVariable nd("VITREO_JIT_NO_DELEGATE", "1");
    const u8 code[] = {0xF0, 0x0F, 0xC0, 0x08};
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    BasicBlock block = build_block(code, sizeof(code));
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    CpuState cpu;
    init_cpu(cpu);
    cpu.gpr.rax = 0x200000;
    cpu.gpr.rcx = 0x42;
    cpu.flags.rflags = 0x202 | 0x1;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    ASSERT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    EXPECT_EQ(cpu.gpr.rcx, 0x42u);
    cpu.materialize_lazy_flags();
    EXPECT_TRUE(cpu.flags.cf());
}

TEST_F(JitParity64Test, SatisfactoryRoundpsRoundpdEveryImmediateAndRcNative) {
    // Intel SDM Vol. 2 ROUNDPS/ROUNDPD: imm8[1:0] mode unless imm8[2] picks
    // MXCSR.RC; imm8[3] suppresses Precision (MXCSR is compared too).
    ScopedEnvironmentVariable nd("VITREO_JIT_NO_DELEGATE", "1");
    float memf[8] = {2.5f, -2.5f, 0.49999997f, 1e30f, -0.0f, 3.0f, 1.5f, -7.75f};
    ASSERT_TRUE(m_memory->write_bytes(kSatData, reinterpret_cast<u8*>(memf), sizeof(memf)));
    struct Enc { const char* name; bool vex; bool l; };
    for (bool dbl : {false, true}) {
        for (Enc e : {Enc{"legacy", false, false}, Enc{"vex128", true, false},
                      Enc{"vex256", true, true}}) {
            for (u32 imm = 0; imm < 16; ++imm) {
                for (u32 rc = 0; rc < 4; ++rc) {
                    for (bool mem_form : {false, true}) {
                        SCOPED_TRACE(::testing::Message() << e.name << " pd=" << dbl
                                     << " imm=" << imm << " rc=" << rc << " mem=" << mem_form);
                        m_jit = std::make_unique<JitCompiler>(*m_memory);
                        std::vector<u8> code;
                        const u8 op = dbl ? 0x09 : 0x08;
                        if (e.vex) {
                            code = vex3(3, 1, e.l, false, 0, op, 1, 2, mem_form,
                                        {static_cast<u8>(imm)});
                        } else {
                            code = {0x66, 0x0F, 0x3A, op,
                                    static_cast<u8>(mem_form ? 0x08 : 0xCA),
                                    static_cast<u8>(imm)};
                        }
                        check_parity(code.data(), code.size(), [&](CpuState& c) {
                            c.gpr.rax = kSatData;
                            c.sse.mxcsr = 0x1F80 | (rc << 13);
                            if (dbl) {
                                const double v[4] = {2.5, -3.5, 0.5000000001, -1e300};
                                c.sse.xmm[2].f64_[0] = v[0]; c.sse.xmm[2].f64_[1] = v[1];
                                c.avx.ymm_high[2].f64_[0] = v[2];
                                c.avx.ymm_high[2].u64_[1] = 0x7FF4000000000001ull;  // SNaN
                            } else {
                                const float v[8] = {0.5f, 1.5f, -0.5f, -1.5f,
                                    std::numeric_limits<float>::infinity(),
                                    std::numeric_limits<float>::quiet_NaN(), 8388609.0f, -2.25f};
                                for (int i = 0; i < 4; ++i) {
                                    c.sse.xmm[2].f32_[i] = v[i];
                                    c.avx.ymm_high[2].f32_[i] = v[4 + i];
                                }
                            }
                        }, false, true, true);
                    }
                }
            }
        }
    }
}

TEST_F(JitParity64Test, SatisfactoryRoundpsModeZeroIgnoresMxcsrRc) {
    // imm8 = 0 is round-to-nearest-even regardless of MXCSR.RC (only imm8[2]
    // consults RC): 2.5 -> 2, 3.5 -> 4, -2.5 -> -2 with RC = round-down.
    ScopedEnvironmentVariable nd("VITREO_JIT_NO_DELEGATE", "1");
    check_parity({0x66, 0x0F, 0x3A, 0x08, 0xCA, 0x00}, [](CpuState& c) {
        c.sse.mxcsr = 0x1F80 | (1u << 13);
        c.sse.xmm[2].f32_[0] = 2.5f; c.sse.xmm[2].f32_[1] = 3.5f;
        c.sse.xmm[2].f32_[2] = -2.5f; c.sse.xmm[2].f32_[3] = 0.25f;
    }, false, true, true);
    for (const CpuState* s : {&m_last_interp_cpu, &m_last_jit_cpu}) {
        EXPECT_EQ(s->sse.xmm[1].f32_[0], 2.0f);
        EXPECT_EQ(s->sse.xmm[1].f32_[1], 4.0f);
        EXPECT_EQ(s->sse.xmm[1].f32_[2], -2.0f);
        EXPECT_EQ(s->sse.xmm[1].f32_[3], 0.0f);
        EXPECT_NE(s->sse.mxcsr & 0x20u, 0u);   // inexact, imm8[3] = 0
    }
}

// ---- VEXTRACTF128 / VEXTRACTI128: every destination form --------------------
// Intel SDM Vol. 2 VEXTRACTF128 / VEXTRACTI128 (VEX.256.66.0F3A.W0 19 / 39
// /r ib): DEST[127:0] := imm8[0] ? SRC[255:128] : SRC[127:0]; the other seven
// immediate bits are ignored. The register destination form also sets
// DEST[MAXVL-1:128] := 0; the memory form is a 16-byte store with no
// alignment requirement. #UD if VEX.L = 0 or VEX.vvvv != 1111b. Expected
// bytes below come from that text applied to the seeded input.
namespace {

u8 extract_seed_byte(int reg, bool hi, int b) {
    const u8 lo = static_cast<u8>(0x10 * reg + b);
    return hi ? static_cast<u8>(0xFF - lo) : lo;
}

void seed_extract64(CpuState& c) {
    for (int r = 0; r < 16; ++r) {
        for (int b = 0; b < 16; ++b) {
            c.sse.xmm[r].u8_[b] = extract_seed_byte(r, false, b);
            c.avx.ymm_high[r].u8_[b] = extract_seed_byte(r, true, b);
        }
    }
    // Every arithmetic flag set, so a lowering that lets its memory probe or
    // helper call clobber host NZCV shows up in the flag comparison.
    c.flags.rflags = 0x202 | 0x1 | 0x4 | 0x40 | 0x80 | 0x800;
}

}  // namespace

TEST_F(JitParity64Test, VextractRegisterDestinationEveryLaneNative) {
    ScopedEnvironmentVariable nd("VITREO_JIT_NO_DELEGATE", "1");
    struct Reg { u8 b1; u8 modrm; int dst; int src; };
    // C4 [R X B 00011] [W0 1111 L1 01] op modrm imm8; rm = DEST, reg = SRC.
    const Reg regs[] = {
        {0xE3, 0xC8, 0, 1},    // xmm0, ymm1
        {0x43, 0xCC, 12, 9},   // xmm12, ymm9 (VEX.R and VEX.B)
        {0xE3, 0xDB, 3, 3},    // xmm3, ymm3 (DEST == SRC)
    };
    for (u8 op : {u8{0x19}, u8{0x39}}) {
        for (const Reg& r : regs) {
            for (u8 imm : {u8{0x00}, u8{0x01}, u8{0xFE}, u8{0xFF}}) {
                SCOPED_TRACE(::testing::Message() << "op=" << std::hex << +op
                             << " dst=" << r.dst << " src=" << r.src << " imm=" << +imm);
                m_jit = std::make_unique<JitCompiler>(*m_memory);
                check_parity({0xC4, r.b1, 0x7D, op, r.modrm, imm}, seed_extract64,
                             true, true, true);
                ASSERT_TRUE(m_last_parity_ok);
                const bool hi = imm & 1;
                for (const CpuState* s : {&m_last_interp_cpu, &m_last_jit_cpu}) {
                    for (int b = 0; b < 16; ++b) {
                        EXPECT_EQ(s->sse.xmm[r.dst].u8_[b], extract_seed_byte(r.src, hi, b));
                        EXPECT_EQ(s->avx.ymm_high[r.dst].u8_[b], 0u);
                    }
                }
            }
        }
    }
}

TEST_F(JitParity64Test, VextractMemoryDestinationEveryAddressingFormNative) {
    ScopedEnvironmentVariable nd("VITREO_JIT_NO_DELEGATE", "1");
    m_compare_memory = true;
    struct Form {
        const char* name;
        int src;
        // Encodes the instruction (placed at 0x1000) for a target EA.
        std::function<std::vector<u8>(u8 op, u8 imm, GuestAddr ea)> code;
        std::function<void(CpuState&, GuestAddr ea)> regs;
    };
    auto le32 = [](std::vector<u8>& v, u32 x) {
        for (int i = 0; i < 4; ++i) v.push_back(static_cast<u8>(x >> (8 * i)));
    };
    const std::vector<Form> forms = {
        {"[rax]", 1,
         [](u8 op, u8 imm, GuestAddr) { return std::vector<u8>{0xC4, 0xE3, 0x7D, op, 0x08, imm}; },
         [](CpuState& c, GuestAddr ea) { c.gpr.rax = ea; }},
        {"[rax+rcx*4+0x10]", 2,
         [](u8 op, u8 imm, GuestAddr) {
             return std::vector<u8>{0xC4, 0xE3, 0x7D, op, 0x54, 0x88, 0x10, imm};
         },
         [](CpuState& c, GuestAddr ea) { c.gpr.rcx = 3; c.gpr.rax = ea - 0x1C; }},
        {"[rip+disp32]", 3,
         [le32](u8 op, u8 imm, GuestAddr ea) {
             // 10 bytes at 0x1000: the disp is relative to 0x100A.
             std::vector<u8> v{0xC4, 0xE3, 0x7D, op, 0x1D};
             le32(v, static_cast<u32>(ea - 0x100A));
             v.push_back(imm);
             return v;
         },
         [](CpuState&, GuestAddr) {}},
        {"[r9+disp32] wraps", 9,
         [le32](u8 op, u8 imm, GuestAddr) {
             std::vector<u8> v{0xC4, 0x43, 0x7D, op, 0x89};
             le32(v, 0x00012340u);
             v.push_back(imm);
             return v;
         },
         [](CpuState& c, GuestAddr ea) { c.gpr.set64(R9, ea - 0x12340); }},
        {"[r12] (SIB, no index)", 4,
         [](u8 op, u8 imm, GuestAddr) {
             return std::vector<u8>{0xC4, 0xC3, 0x7D, op, 0x24, 0x24, imm};
         },
         [](CpuState& c, GuestAddr ea) { c.gpr.set64(12, ea); }},
        {"[rdx], ymm15", 15,
         [](u8 op, u8 imm, GuestAddr) { return std::vector<u8>{0xC4, 0x63, 0x7D, op, 0x3A, imm}; },
         [](CpuState& c, GuestAddr ea) { c.gpr.rdx = ea; }},
        {"fs:[rbx]", 5,
         [](u8 op, u8 imm, GuestAddr) {
             return std::vector<u8>{0x64, 0xC4, 0xE3, 0x7D, op, 0x2B, imm};
         },
         [](CpuState& c, GuestAddr ea) { c.seg.fs_desc.base = 0x7000; c.gpr.rbx = ea - 0x7000; }},
        {"gs:[rbx]", 6,
         [](u8 op, u8 imm, GuestAddr) {
             return std::vector<u8>{0x65, 0xC4, 0xE3, 0x7D, op, 0x33, imm};
         },
         [](CpuState& c, GuestAddr ea) { c.seg.gs_desc.base = 0x6000; c.gpr.rbx = ea - 0x6000; }},
        {"cmp; [rax] (live flags)", 1,
         [](u8 op, u8 imm, GuestAddr) {
             return std::vector<u8>{0x48, 0x39, 0xC1, 0xC4, 0xE3, 0x7D, op, 0x08, imm};
         },
         [](CpuState& c, GuestAddr ea) { c.gpr.rax = ea; c.gpr.rcx = ea + 1; }},
    };
    // Aligned, unaligned, across a 4 KiB guest page and across a 16 KiB host
    // page, all inside the mapping.
    const GuestAddr eas[] = {0x8000, 0x8005, 0x8FF8, 0xBFF9};
    for (const Form& f : forms) {
        for (u8 op : {u8{0x19}, u8{0x39}}) {
            for (u8 imm : {u8{0x00}, u8{0x01}, u8{0xFE}, u8{0xFF}}) {
                for (GuestAddr ea : eas) {
                    SCOPED_TRACE(::testing::Message() << f.name << " op=" << std::hex << +op
                                 << " imm=" << +imm << " ea=" << ea);
                    std::vector<u8> fill(48);
                    for (usize i = 0; i < fill.size(); ++i) fill[i] = static_cast<u8>(0xC0 ^ i);
                    ASSERT_TRUE(m_memory->write_bytes(ea - 16, fill.data(), fill.size()));
                    const auto code = f.code(op, imm, ea);
                    m_jit = std::make_unique<JitCompiler>(*m_memory);
                    check_parity(code.data(), code.size(), [&](CpuState& c) {
                        seed_extract64(c);
                        f.regs(c, ea);
                    }, true, true, true);
                    ASSERT_TRUE(m_last_parity_ok);
                    // Memory now holds the JIT's result.
                    std::vector<u8> got(48);
                    ASSERT_TRUE(m_memory->read_bytes(ea - 16, got.data(), got.size()));
                    for (int b = 0; b < 16; ++b) {
                        EXPECT_EQ(got[16 + b], extract_seed_byte(f.src, imm & 1, b)) << "byte " << b;
                        EXPECT_EQ(got[b], fill[b]) << "below the store";
                        EXPECT_EQ(got[32 + b], fill[32 + b]) << "above the store";
                    }
                    // The source register is read, never written.
                    for (int b = 0; b < 16; ++b) {
                        EXPECT_EQ(m_last_jit_cpu.sse.xmm[f.src].u8_[b], extract_seed_byte(f.src, false, b));
                        EXPECT_EQ(m_last_jit_cpu.avx.ymm_high[f.src].u8_[b], extract_seed_byte(f.src, true, b));
                    }
                }
            }
        }
    }
}

// A store denied anywhere in its 16 bytes performs none of itself (Intel SDM
// Vol. 3A 6.15) and raises STATUS_ACCESS_VIOLATION at the instruction; the
// reported address is the first inaccessible byte (Windows, measured by
// tools/oracle_probe/split_access_fault_address_probe).
TEST_F(JitParity64Test, VextractMemoryFaultStoresNothingNative) {
    ScopedEnvironmentVariable nd("VITREO_JIT_NO_DELEGATE", "1");
    for (u8 op : {u8{0x19}, u8{0x39}}) {
        for (u8 imm : {u8{0x00}, u8{0x01}}) {
            // Leading CMP keeps guest flags live across the faulting store.
            const std::vector<u8> code = {0x48, 0x39, 0xC1, 0xC4, 0xE3, 0x7D, op, 0x08, imm};
            ASSERT_TRUE(m_memory->write_bytes(0x1000, code.data(), code.size()));
            m_jit = std::make_unique<JitCompiler>(*m_memory);
            BasicBlock block = build_block(code.data(), code.size());
            ASSERT_EQ(block.instruction_count, 2u);
            CompiledBlock* compiled = m_jit->compile(block);
            ASSERT_NE(compiled, nullptr);
            for (u32 writable : {0u, 1u, 8u, 15u}) {
                SCOPED_TRACE(::testing::Message() << "op=" << std::hex << +op
                             << " imm=" << +imm << " writable=" << std::dec << writable);
                const GuestAddr ea = 0x11000 - writable;   // mapping ends at 0x11000
                std::vector<u8> before(16, 0x77);
                if (writable) ASSERT_TRUE(m_memory->write_bytes(ea, before.data(), writable));
                CpuState cpu;
                init_cpu(cpu);
                seed_extract64(cpu);
                cpu.gpr.rax = ea;
                cpu.gpr.rcx = 5;

                CpuState interpreted = cpu;
                {
                    Interpreter interpreter(interpreted, *m_memory);
                    EXPECT_EQ(interpreter.step(), ExecResult::Continue);
                    EXPECT_EQ(interpreter.step(), ExecResult::Exception);
                }
                std::vector<u8> after(16, 0);
                if (writable) {
                    ASSERT_TRUE(m_memory->read_bytes(ea, after.data(), writable));
                    for (u32 i = 0; i < writable; ++i) EXPECT_EQ(after[i], 0x77u) << "interp byte " << i;
                }

                EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu),
                          JitExecResult::Continue);
                ASSERT_TRUE(cpu.pending_raise.active);
                EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
                EXPECT_EQ(cpu.pending_raise.args[0], 1u);          // write
                EXPECT_EQ(cpu.pending_raise.args[1], 0x11000u);    // first denied byte
                EXPECT_EQ(cpu.rip, 0x1003u);                       // the VEXTRACT
                if (writable) {
                    ASSERT_TRUE(m_memory->read_bytes(ea, after.data(), writable));
                    for (u32 i = 0; i < writable; ++i) EXPECT_EQ(after[i], 0x77u) << "jit byte " << i;
                }
                // CMP RCX(5), RAX: flags of the completed CMP survive the fault.
                cpu.materialize_lazy_flags();
                interpreted.materialize_lazy_flags();
                EXPECT_EQ(cpu.flags.cf(), interpreted.flags.cf());
                EXPECT_EQ(cpu.flags.zf(), interpreted.flags.zf());
                EXPECT_EQ(cpu.flags.sf(), interpreted.flags.sf());
                EXPECT_EQ(cpu.flags.of(), interpreted.flags.of());
                EXPECT_TRUE(cpu.flags.cf());                       // 5 < ea, unsigned
            }
        }
    }
}

TEST_F(JitParity64Test, VextractUndefinedEncodingsDoNotDecode) {
    // VEX.L = 0 and VEX.vvvv != 1111b are #UD (Intel SDM Vol. 2). The decoder
    // is shared by both engines, so rejecting the encoding is the contract.
    const std::vector<std::vector<u8>> undefined = {
        {0xC4, 0xE3, 0x79, 0x19, 0xC8, 0x00},   // VEXTRACTF128, L = 0
        {0xC4, 0xE3, 0x79, 0x39, 0xC8, 0x01},   // VEXTRACTI128, L = 0
        {0xC4, 0xE3, 0x79, 0x19, 0x08, 0x00},   // L = 0, memory destination
        {0xC4, 0xE3, 0x75, 0x19, 0xC8, 0x00},   // vvvv = 1110b (ymm1)
        {0xC4, 0xE3, 0x05, 0x19, 0xC8, 0x00},   // vvvv = 0000b (ymm15)
    };
    auto rejects = [](const std::vector<u8>& code, DecodeMode mode) {
        MemoryByteReader reader(code.data(), code.size());
        DecodedInstruction inst;
        EXPECT_NE(decode_instruction(reader, mode, inst), DecodeError::Success)
            << "mode=" << static_cast<int>(mode) << " byte2=0x" << std::hex << +code[2]
            << " op=0x" << +code[3];
    };
    for (const auto& code : undefined) {
        rejects(code, DecodeMode::Mode64);
        rejects(code, DecodeMode::Mode32);
    }
    // vvvv = 0111b (ymm8). Long mode only: whether the inverted VEX.vvvv[3]
    // is significant outside 64-bit mode is not asserted here.
    rejects({0xC4, 0xE3, 0x3D, 0x39, 0x08, 0x01}, DecodeMode::Mode64);
    // The defined encoding still decodes in both modes.
    for (DecodeMode mode : {DecodeMode::Mode64, DecodeMode::Mode32}) {
        const u8 ok[] = {0xC4, 0xE3, 0x7D, 0x19, 0xC8, 0x01};
        MemoryByteReader reader(ok, sizeof(ok));
        DecodedInstruction inst;
        ASSERT_EQ(decode_instruction(reader, mode, inst), DecodeError::Success);
        EXPECT_EQ(inst.opcode, Opcode::VEXTRACTF128_XMMM128_YMM_IMM8);
    }
}

// ---------------------------------------------------------------------------
// Faulting 16- and 32-byte vector stores in a 64-bit block: interpreter vs
// JIT and both against Windows (simd_store_fault_cases.hpp). Run with and
// without VITREO_TEST_PARITY_LINEAR64. Boundaries: inside a host page (the
// code region's end), on a 16 KiB host-page edge, and before read-only pages
// on both kinds of edge.
// ---------------------------------------------------------------------------
#include "simd_store_fault_cases.hpp"

TEST_F(JitParity64Test, VectorStoreFaultsMatchInterpreterAndWindows) {
    ScopedEnvironmentVariable nd("VITREO_JIT_NO_DELEGATE", "1");
    ASSERT_TRUE(m_memory->map_region(0x20000, 0x4000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->map_region(0x30000, 0x4000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->map_region(0x34000, 0x4000, MemoryProtection::Read));
    ASSERT_TRUE(m_memory->map_region(0x40000, 0x1000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->map_region(0x41000, 0x1000, MemoryProtection::Read));
    GuestMemory& mem = *m_memory;
    const vitreo::cpu::test_simd_store::Env env{
        mem, 0x1000, true,
        [this](const std::vector<u8>& code) { return build_block(code.data(), code.size()); },
        [this](CpuState& c) { init_cpu(c); },
        [&mem](const BasicBlock& block, CpuState& cpu) {
            JitCompiler jit(mem);
            CompiledBlock* cb = jit.compile(block);
            if (cb == nullptr) {
                ADD_FAILURE() << "store block did not compile natively";
                return JitExecResult::StaleCode;
            }
            return test::execute_like_dispatcher(jit, *cb, cpu);
        }};
    for (GuestAddr b : {GuestAddr{0x11000}, GuestAddr{0x24000}, GuestAddr{0x34000},
                        GuestAddr{0x41000}}) {
        SCOPED_TRACE(::testing::Message() << "boundary 0x" << std::hex << b);
        vitreo::cpu::test_simd_store::run_all_forms(env, b);
    }
}
