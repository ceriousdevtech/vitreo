// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests — 64-bit long mode (CPU-048): float-to-int
// conversions and AVX broadcasts.

#include "jit_parity_64_fixture.hpp"

// ============================================================================
// 64-bit-destination float→int conversions (REX.W CVT{,T}SS2SI / CVT{,T}SD2SI)
//
// These were interpreter delegates until the compile_sse arms landed. The
// valuable cases are the ones a lowering copied from the r32 arm gets wrong:
// a source that is in range for i64 but not for i32 (1.0e10), and a store
// that must be full-width (hence the 0xFFFF... seed in RAX).
// ============================================================================

namespace {

// CVTSS2SI RAX, XMM1   — F3 48 0F 2D C1
constexpr std::initializer_list<u8> kCvtss2siRax = {0xF3, 0x48, 0x0F, 0x2D, 0xC1};
// CVTTSS2SI RAX, XMM1  — F3 48 0F 2C C1
constexpr std::initializer_list<u8> kCvttss2siRax = {0xF3, 0x48, 0x0F, 0x2C, 0xC1};
// CVTSD2SI RAX, XMM1   — F2 48 0F 2D C1
constexpr std::initializer_list<u8> kCvtsd2siRax = {0xF2, 0x48, 0x0F, 0x2D, 0xC1};
// CVTTSD2SI RAX, XMM1  — F2 48 0F 2C C1
constexpr std::initializer_list<u8> kCvttsd2siRax = {0xF2, 0x48, 0x0F, 0x2C, 0xC1};

std::function<void(CpuState&)> seed_ss(f32 v, u32 mxcsr = 0x1F80u) {
    return [v, mxcsr](CpuState& cpu) {
        cpu.gpr.set64(RAX, 0xFFFFFFFFFFFFFFFFull);
        cpu.sse.mxcsr = mxcsr;
        cpu.sse.xmm[1].f32_[0] = v;
    };
}

std::function<void(CpuState&)> seed_sd(f64 v, u32 mxcsr = 0x1F80u) {
    return [v, mxcsr](CpuState& cpu) {
        cpu.gpr.set64(RAX, 0xFFFFFFFFFFFFFFFFull);
        cpu.sse.mxcsr = mxcsr;
        cpu.sse.xmm[1].f64_[0] = v;
    };
}

}  // namespace

TEST_F(JitParity64Test, Cvtss2siR64Values) {
    for (f32 v : {3.7f, -3.7f, 1.0e10f, 1.0e19f,
                  std::numeric_limits<f32>::quiet_NaN(),
                  -std::numeric_limits<f32>::infinity()}) {
        SCOPED_TRACE(v);
        check_native_parity(kCvtss2siRax, seed_ss(v), false, true);
    }
}

TEST_F(JitParity64Test, Cvttss2siR64Values) {
    for (f32 v : {3.7f, -3.7f, 1.0e10f, 1.0e19f,
                  std::numeric_limits<f32>::quiet_NaN(),
                  -std::numeric_limits<f32>::infinity()}) {
        SCOPED_TRACE(v);
        check_native_parity(kCvttss2siRax, seed_ss(v), false, true);
    }
}

TEST_F(JitParity64Test, Cvtsd2siR64Values) {
    for (f64 v : {3.7, -3.7, 1.0e10, 1.0e19,
                  std::numeric_limits<f64>::quiet_NaN(),
                  -std::numeric_limits<f64>::infinity()}) {
        SCOPED_TRACE(v);
        check_native_parity(kCvtsd2siRax, seed_sd(v), false, true);
    }
}

TEST_F(JitParity64Test, Cvttsd2siR64Values) {
    for (f64 v : {3.7, -3.7, 1.0e10, 1.0e19,
                  std::numeric_limits<f64>::quiet_NaN(),
                  -std::numeric_limits<f64>::infinity()}) {
        SCOPED_TRACE(v);
        check_native_parity(kCvttsd2siRax, seed_sd(v), false, true);
    }
}

// The recorded divergence "CVTSS2SI ignores MXCSR rounding" (FP oracle vs real
// Windows) is exactly this: the non-truncating forms must route through
// emit_fcvt_scalar_by_mxcsr, so all four RC values have to be exercised. -3.7
// separates every mode: nearest=-4, down=-4, up=-3, zero=-3.
TEST_F(JitParity64Test, Cvtss2siR64HonoursMxcsrRoundingControl) {
    for (u32 rc = 0; rc < 4; ++rc) {
        SCOPED_TRACE(rc);
        check_native_parity(kCvtss2siRax,
                            seed_ss(-3.7f, 0x1F80u | (rc << 13)), false, true);
    }
}

TEST_F(JitParity64Test, Cvtsd2siR64HonoursMxcsrRoundingControl) {
    for (u32 rc = 0; rc < 4; ++rc) {
        SCOPED_TRACE(rc);
        check_native_parity(kCvtsd2siRax,
                            seed_sd(-3.7, 0x1F80u | (rc << 13)), false, true);
    }
}


// ============================================================================
// AVX/AVX2 memory-form broadcasts (VBROADCASTSS/SD/F128/I128)
//
// The register forms have been lowered natively for a while; the memory forms
// used to fall through to the interpreter delegate. These use
// check_native_parity, so compile() returning nullptr (i.e. still delegating)
// is a hard failure, not a skip.
//
// Every source operand sits at 0x140006000 — deliberately above 4 GB. The EA
// for these instructions is 64-bit in long mode, and a 32-bit MOV anywhere on
// that path silently drops bits 63:32; at a low test address such a bug is
// invisible. Two of the tests also use an indexed [rax+rcx*4] operand, which
// is what makes emit_compute_ea64 claim REG_EA_TMP (X5) as scratch.
// ============================================================================

namespace {
constexpr GuestAddr kBroadcastSrc = 0x140006000ull;
}  // namespace

TEST_F(JitParity64Test, VbroadcastssXmmM32AboveFourGigZeroesUpperLane) {
    map_tracked_region(kBroadcastSrc, 0x1000);
    static const u8 kData[16] = {
        0x11, 0x22, 0x33, 0x44,  0x55, 0x66, 0x77, 0x88,
        0x99, 0xAA, 0xBB, 0xCC,  0xDD, 0xEE, 0xFF, 0x00,
    };
    ASSERT_TRUE(m_memory->write_bytes(kBroadcastSrc, kData, sizeof(kData)));

    // vbroadcastss xmm1, [rax]  — VEX.128.66.0F38.W0 18 /r
    check_native_parity({0xC4, 0xE2, 0x79, 0x18, 0x08},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, kBroadcastSrc);
            seed_ymm_dirty(cpu, 1);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParity64Test, VbroadcastssYmmM32IndexedAboveFourGig) {
    map_tracked_region(kBroadcastSrc, 0x1000);
    static const u8 kData[16] = {
        0x11, 0x22, 0x33, 0x44,  0x55, 0x66, 0x77, 0x88,
        0x99, 0xAA, 0xBB, 0xCC,  0xDD, 0xEE, 0xFF, 0x00,
    };
    ASSERT_TRUE(m_memory->write_bytes(kBroadcastSrc, kData, sizeof(kData)));

    // vbroadcastss ymm2, [rax+rcx*4]  — VEX.256.66.0F38.W0 18 /r, SIB 0x88.
    // RCX = 2 selects the dword at +8. The indexed form is what drives
    // emit_compute_ea64 onto its REG_EA_TMP (X5) path.
    check_native_parity({0xC4, 0xE2, 0x7D, 0x18, 0x14, 0x88},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, kBroadcastSrc);
            cpu.gpr.set64(RCX, 2);
            seed_ymm_dirty(cpu, 2);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParity64Test, VbroadcastsdYmmM64AboveFourGig) {
    map_tracked_region(kBroadcastSrc, 0x1000);
    static const u8 kData[16] = {
        0x0F, 0x1E, 0x2D, 0x3C,  0x4B, 0x5A, 0x69, 0x78,
        0x87, 0x96, 0xA5, 0xB4,  0xC3, 0xD2, 0xE1, 0xF0,
    };
    ASSERT_TRUE(m_memory->write_bytes(kBroadcastSrc, kData, sizeof(kData)));

    // vbroadcastsd ymm3, [rax+8]  — VEX.256.66.0F38.W0 19 /r, mod=01 disp8.
    check_native_parity({0xC4, 0xE2, 0x7D, 0x19, 0x58, 0x08},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, kBroadcastSrc);
            seed_ymm_dirty(cpu, 3);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParity64Test, Vbroadcastf128YmmM128AboveFourGig) {
    map_tracked_region(kBroadcastSrc, 0x1000);
    static const u8 kData[16] = {
        0x01, 0x23, 0x45, 0x67,  0x89, 0xAB, 0xCD, 0xEF,
        0xFE, 0xDC, 0xBA, 0x98,  0x76, 0x54, 0x32, 0x10,
    };
    ASSERT_TRUE(m_memory->write_bytes(kBroadcastSrc, kData, sizeof(kData)));

    // vbroadcastf128 ymm4, [rax]  — VEX.256.66.0F38.W0 1A /r.
    // Two 8-byte guest loads at EA and EA+8; the second load must recompute
    // the address from the parked 64-bit copy, not from a truncated one.
    check_native_parity({0xC4, 0xE2, 0x7D, 0x1A, 0x20},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, kBroadcastSrc);
            seed_ymm_dirty(cpu, 4);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParity64Test, Vbroadcasti128YmmM128IndexedAboveFourGig) {
    map_tracked_region(kBroadcastSrc, 0x1000);
    static const u8 kData[32] = {
        0x01, 0x23, 0x45, 0x67,  0x89, 0xAB, 0xCD, 0xEF,
        0xFE, 0xDC, 0xBA, 0x98,  0x76, 0x54, 0x32, 0x10,
        0xC0, 0xFF, 0xEE, 0x00,  0xDE, 0xAD, 0xBE, 0xEF,
        0xBA, 0xAD, 0xF0, 0x0D,  0x13, 0x37, 0x42, 0x99,
    };
    ASSERT_TRUE(m_memory->write_bytes(kBroadcastSrc, kData, sizeof(kData)));

    // vbroadcasti128 ymm5, [rax+rcx*4]  — VEX.256.66.0F38.W0 5A /r, SIB 0x88.
    // RCX = 4 selects the 16 bytes at +16, so an EA computed without the
    // scaled index reads the wrong half and the values diverge.
    check_native_parity({0xC4, 0xE2, 0x7D, 0x5A, 0x2C, 0x88},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, kBroadcastSrc);
            cpu.gpr.set64(RCX, 4);
            seed_ymm_dirty(cpu, 5);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParity64Test, VpbroadcastByteMemoryXmmAndYmmAboveFourGig) {
    map_tracked_region(kBroadcastSrc, 0x1000);
    const u8 data[] = {0x3C, 0xA7};
    ASSERT_TRUE(m_memory->write_bytes(kBroadcastSrc, data, sizeof(data)));

    // vpbroadcastb xmm1,[rax]; vpbroadcastb ymm2,[rax+1]
    check_native_parity({0xC4, 0xE2, 0x79, 0x78, 0x08,
                         0xC4, 0xE2, 0x7D, 0x78, 0x50, 0x01},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, kBroadcastSrc);
            seed_ymm_dirty(cpu, 1);
            seed_ymm_dirty(cpu, 2);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParity64Test, VpbroadcastWordMemoryXmmAndYmmAboveFourGig) {
    map_tracked_region(kBroadcastSrc, 0x1000);
    const u8 data[] = {0x34, 0x12, 0xCD, 0xAB};
    ASSERT_TRUE(m_memory->write_bytes(kBroadcastSrc, data, sizeof(data)));

    // vpbroadcastw xmm3,[rax]; vpbroadcastw ymm4,[rax+2]
    check_native_parity({0xC4, 0xE2, 0x79, 0x79, 0x18,
                         0xC4, 0xE2, 0x7D, 0x79, 0x60, 0x02},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, kBroadcastSrc);
            seed_ymm_dirty(cpu, 3);
            seed_ymm_dirty(cpu, 4);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParity64Test, VpbroadcastDwordMemoryXmmAndIndexedYmmAboveFourGig) {
    map_tracked_region(kBroadcastSrc, 0x1000);
    const u8 data[] = {
        0x78, 0x56, 0x34, 0x12, 0xEF, 0xCD, 0xAB, 0x90,
    };
    ASSERT_TRUE(m_memory->write_bytes(kBroadcastSrc, data, sizeof(data)));

    // vpbroadcastd xmm5,[rax]; vpbroadcastd ymm6,[rax+rcx*4]
    check_native_parity({0xC4, 0xE2, 0x79, 0x58, 0x28,
                         0xC4, 0xE2, 0x7D, 0x58, 0x34, 0x88},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, kBroadcastSrc);
            cpu.gpr.set64(RCX, 1);
            seed_ymm_dirty(cpu, 5);
            seed_ymm_dirty(cpu, 6);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParity64Test, VpbroadcastQwordMemoryXmmAndYmmAboveFourGig) {
    map_tracked_region(kBroadcastSrc, 0x1000);
    const u8 data[] = {
        0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01,
        0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE,
    };
    ASSERT_TRUE(m_memory->write_bytes(kBroadcastSrc, data, sizeof(data)));

    // vpbroadcastq xmm7,[rax]; vpbroadcastq ymm0,[rax+8]
    check_native_parity({0xC4, 0xE2, 0x79, 0x59, 0x38,
                         0xC4, 0xE2, 0x7D, 0x59, 0x40, 0x08},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, kBroadcastSrc);
            seed_ymm_dirty(cpu, 7);
            seed_ymm_dirty(cpu, 0);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

// FNSTSW AX writes AX only: bits 63:16 of RAX must survive. In a 64-bit block
// RAX is pinned in X23, so this now inserts straight into the pin with a
// 64-bit BFI instead of spilling the pin to CpuState, storing 16 bits, and
// reloading it. The W-form of BFI would zero bits 63:32 -- hence the seed.
TEST_F(JitParity64Test, FnstswAxWritesPinAndPreservesRaxHighBits) {
    check_native_parity({0xDF, 0xE0},                    // fnstsw ax
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xDEADBEEFCAFEBABEull);
            cpu.fpu.status_word = 0x4321;
        },
        /*check_flags=*/false);
}

// Same, but with the status word produced by a real compare, and with a second
// pinned register seeded so a stray wrong-width pin spill would show up.
TEST_F(JitParity64Test, FcomppThenFnstswAxKeepsRaxHighBits) {
    // FLD1 ; FLDZ ; FCOMPP ; FNSTSW AX
    check_native_parity({0xD9, 0xE8, 0xD9, 0xEE, 0xDE, 0xD9, 0xDF, 0xE0},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x1122334455667788ull);
            cpu.gpr.set64(RCX, 0xFFEEDDCCBBAA9988ull);
        },
        /*check_flags=*/false);
}

TEST_F(JitParity64Test, X87FcomppClearsC1AndPreservesFullStateAndEflags) {
    // CMP ECX,ECX ; FLD1 ; FLDZ ; FCOMPP ; FNSTSW AX ; SETZ BL.
    // TOP/tags/C bits and pending integer flags remain architectural across
    // the native Float80 reconstruction and ARM FCMP.
    check_native_parity({0x39, 0xC9,
                         0xD9, 0xE8, 0xD9, 0xEE, 0xDE, 0xD9,
                         0xDF, 0xE0, 0x0F, 0x94, 0xC3},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x1122334455667788ull);
            cpu.gpr.set64(RBX, 0xFFEEDDCCBBAA9900ull);
            cpu.gpr.set64(RCX, 7);
            // Keep the two push destinations (physical 7 and 6) Empty while
            // seeding unrelated live tags and C1=1. FCOMPP must clear C1,
            // restore TOP, and leave all unrelated physical tags untouched.
            cpu.fpu.status_word = FPU_SW_C1;
            cpu.fpu.tag_word = static_cast<u16>(
                (0xFFFFu & ~u16{0x0003u} & ~u16{0x00C0u}) | u16{0x0040u});
            cpu.fpu.st[0] = Float80::from_extended(
                0xC000000000000000ull, 0x4000);
            cpu.fpu.st[3] = Float80{};
        });
    ASSERT_TRUE(m_last_parity_ok);
    EXPECT_EQ(m_last_jit_cpu.gpr.get64(RBX) & 0xFFu, 1u);
    EXPECT_EQ(m_last_jit_cpu.fpu.status_word, 0x0100u);
    EXPECT_EQ(m_last_jit_cpu.fpu.tag_word, 0xFF7Cu);
    EXPECT_EQ(m_last_interp_cpu.fpu.status_word,
              m_last_jit_cpu.fpu.status_word);
    EXPECT_EQ(m_last_interp_cpu.fpu.tag_word,
              m_last_jit_cpu.fpu.tag_word);
    EXPECT_EQ(std::memcmp(&m_last_interp_cpu.fpu.st[0],
                          &m_last_jit_cpu.fpu.st[0], sizeof(Float80)), 0);
    EXPECT_EQ(std::memcmp(&m_last_interp_cpu.fpu.st[3],
                          &m_last_jit_cpu.fpu.st[3], sizeof(Float80)), 0);
}
