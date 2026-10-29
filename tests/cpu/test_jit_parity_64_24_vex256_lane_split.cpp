// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests — 64-bit long mode (CPU-048): VEX.256 lane
// split, classes A and B.

#include "jit_parity_64_fixture.hpp"

// ============================================================================
// VEX.256 element-wise lane split (class A)
//
// A 256-bit VEX form of PXOR/PAND/POR/ANDPS/PADDD/PSUBD/MINPS/MAXPS is lowered
// by re-emitting the existing 128-bit lowering once per 128-bit lane, with
// CpuState offsets redirected to AvxState::ymm_high on the second pass.
// check_native_parity sets VITREO_JIT_NO_DELEGATE=1 and requires a fully
// native block, so every one of these fails loudly if the opcode falls back to
// the interpreter bridge instead of being lowered.
// ============================================================================
namespace {
// Distinct, asymmetric per-register seed. ymm_high must never equal xmm for
// the same register, or a lane-split bug that reads the wrong half is
// invisible.
void seed_ymm(CpuState& cpu, int reg, u32 lo_base, u32 hi_base) {
    for (int i = 0; i < 4; ++i) {
        cpu.sse.xmm[reg].u32_[i] = lo_base + 0x01010101u * static_cast<u32>(i);
        cpu.avx.ymm_high[reg].u32_[i] =
            hi_base + 0x10101010u * static_cast<u32>(i);
    }
}
// Poison the destination's high lane so "never written" and "written with the
// right value" cannot be confused, and so a VEX.128 form that fails to zero it
// is caught.
void poison_high(CpuState& cpu, int reg) {
    for (int i = 0; i < 4; ++i) cpu.avx.ymm_high[reg].u32_[i] = 0xDEADBEEFu;
}
} // namespace

// (a) VEX.256 register form must write BOTH lanes.
TEST_F(JitParity64Test, Vpaddd256RegWritesBothLanes) {
    // vpaddd ymm0, ymm1, ymm2 — C5 F5 FE C2
    check_native_parity({0xC5, 0xF5, 0xFE, 0xC2},
        [](CpuState& cpu) {
            seed_ymm(cpu, 1, 0x11111111u, 0x22222222u);
            seed_ymm(cpu, 2, 0x33445566u, 0x778899AAu);
            poison_high(cpu, 0);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

// (b) The VEX.128 form of the same opcode must still ZERO YMM[255:128].
// This is the regression guard for `vex_lowered_as_legacy_sse`: the lane-split
// driver must not steal the 128-bit path's high-lane zero store.
TEST_F(JitParity64Test, Vpaddd128StillZeroesHighLane) {
    // vpaddd xmm0, xmm1, xmm2 — C5 F1 FE C2
    check_native_parity({0xC5, 0xF1, 0xFE, 0xC2},
        [](CpuState& cpu) {
            seed_ymm(cpu, 1, 0x11111111u, 0x22222222u);
            seed_ymm(cpu, 2, 0x33445566u, 0x778899AAu);
            poison_high(cpu, 0);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParity64Test, Vandps128StillZeroesHighLane) {
    // vandps xmm0, xmm1, xmm2 — C5 F0 54 C2
    check_native_parity({0xC5, 0xF0, 0x54, 0xC2},
        [](CpuState& cpu) {
            seed_ymm(cpu, 1, 0xF0F0F0F0u, 0xFF00FF00u);
            seed_ymm(cpu, 2, 0x0FF00FF0u, 0x00FFFF00u);
            poison_high(cpu, 0);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParity64Test, Vmaxps128StillZeroesHighLane) {
    // vmaxps xmm0, xmm1, xmm2 — C5 F0 5F C2
    check_native_parity({0xC5, 0xF0, 0x5F, 0xC2},
        [](CpuState& cpu) {
            for (int i = 0; i < 4; ++i) {
                cpu.sse.xmm[1].f32_[i] = static_cast<float>(i) - 1.5f;
                cpu.sse.xmm[2].f32_[i] = 1.25f - static_cast<float>(i);
                cpu.avx.ymm_high[1].f32_[i] = 7.5f;
                cpu.avx.ymm_high[2].f32_[i] = -7.5f;
            }
            poison_high(cpu, 0);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

// (c) dst aliases the r/m source — the recorded `vex_merge_clobbers_rm_source`
// landmine (`SRC1 op SRC1`). Class A never emits a merge copy: SRC1 comes from
// vvvv and the r/m source is loaded into V1 before the destination is stored,
// so this form IS lowered natively and must still be correct. (Note the
// 128-bit path *declines* this shape, because its merge copy would destroy the
// r/m source; the 256-bit path has no merge copy to be destroyed by.)
TEST_F(JitParity64Test, Vpsubd256DstAliasesRmSource) {
    // vpsubd ymm2, ymm1, ymm2 — C5 F5 FA D2
    check_native_parity({0xC5, 0xF5, 0xFA, 0xD2},
        [](CpuState& cpu) {
            seed_ymm(cpu, 1, 0xAAAAAAAAu, 0xBBBBBBBBu);
            seed_ymm(cpu, 2, 0x12345678u, 0x9ABCDEF0u);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParity64Test, Vpaddd256DstAliasesRmSource) {
    // vpaddd ymm2, ymm1, ymm2 — C5 F5 FE D2
    check_native_parity({0xC5, 0xF5, 0xFE, 0xD2},
        [](CpuState& cpu) {
            seed_ymm(cpu, 1, 0xAAAAAAAAu, 0xBBBBBBBBu);
            seed_ymm(cpu, 2, 0x12345678u, 0x9ABCDEF0u);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

// (d) dst aliases vvvv (SRC1). Must compile natively and write both lanes; a
// lane-0 high-lane zero store would destroy the high SRC1 before lane 1 runs.
TEST_F(JitParity64Test, Vpaddd256DstAliasesVvvv) {
    // vpaddd ymm0, ymm0, ymm1 — C5 FD FE C1
    check_native_parity({0xC5, 0xFD, 0xFE, 0xC1},
        [](CpuState& cpu) {
            seed_ymm(cpu, 0, 0x01020304u, 0x05060708u);
            seed_ymm(cpu, 1, 0x10203040u, 0x50607080u);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

// The remaining class-A opcodes, 256-bit register form.
TEST_F(JitParity64Test, Vex256ClassAOpcodesRegForm) {
    struct Case { const char* name; std::initializer_list<u8> code; };
    const Case cases[] = {
        {"vpand  ymm0,ymm1,ymm2", {0xC5, 0xF5, 0xDB, 0xC2}},
        {"vpor   ymm0,ymm1,ymm2", {0xC5, 0xF5, 0xEB, 0xC2}},
        {"vpxor  ymm0,ymm1,ymm2", {0xC5, 0xF5, 0xEF, 0xC2}},
        {"vandps ymm0,ymm1,ymm2", {0xC5, 0xF4, 0x54, 0xC2}},
        {"vpsubd ymm0,ymm1,ymm2", {0xC5, 0xF5, 0xFA, 0xC2}},
        {"vminps ymm0,ymm1,ymm2", {0xC5, 0xF4, 0x5D, 0xC2}},
        {"vmaxps ymm0,ymm1,ymm2", {0xC5, 0xF4, 0x5F, 0xC2}},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        check_native_parity(c.code,
            [](CpuState& cpu) {
                // Finite, asymmetric, and distinct per lane; also valid as
                // integers for the bitwise/PSUBD entries.
                for (int i = 0; i < 4; ++i) {
                    cpu.sse.xmm[1].f32_[i] = static_cast<float>(i) - 1.5f;
                    cpu.sse.xmm[2].f32_[i] = 1.25f - static_cast<float>(i);
                    cpu.avx.ymm_high[1].f32_[i] =
                        3.5f * static_cast<float>(i + 1);
                    cpu.avx.ymm_high[2].f32_[i] =
                        -2.25f * static_cast<float>(i + 1);
                }
                poison_high(cpu, 0);
            },
            /*check_flags=*/false, /*check_xmm=*/true);
    }
}

// (e) Memory operand ABOVE 4 GiB. The high lane reads EA+16, and the +16 must
// go through the full 64-bit EA path — this is the `jit64_ea_truncation`
// family guard.
TEST_F(JitParity64Test, Vpaddd256MemAbove4GiB) {
    constexpr u64 kHigh = 0x1'0000'2000ull;
    map_tracked_region(kHigh, 0x1000);
    for (u32 i = 0; i < 32; ++i)
        ASSERT_TRUE(m_memory->write_u8(kHigh + i, static_cast<u8>(0xA0 + i)));
    // vpaddd ymm0, ymm1, [rbx] — C5 F5 FE 03
    check_native_parity({0xC5, 0xF5, 0xFE, 0x03},
        [](CpuState& cpu) {
            cpu.gpr.set64(RBX, kHigh);
            seed_ymm(cpu, 1, 0x11111111u, 0x22222222u);
            poison_high(cpu, 0);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

// …and with the region placed so that lane 1's +16 crosses a 32-bit boundary.
// A 32-bit ADD for the displacement would wrap to 0x1'0000'0000 here.
TEST_F(JitParity64Test, Vminps256MemHighLaneCrosses32BitBoundary) {
    constexpr u64 kRegion = 0x1'FFFF'F000ull;
    constexpr u64 kAddr = 0x1'FFFF'FFF0ull;   // +16 == 0x2'0000'0000
    map_tracked_region(kRegion, 0x2000);
    for (u32 i = 0; i < 32; ++i) {
        const float v = 0.5f * static_cast<float>(i) - 3.0f;
        u32 bits;
        std::memcpy(&bits, &v, 4);
        if ((i & 3) == 0) ASSERT_TRUE(m_memory->write_u32(kAddr + i, bits));
    }
    // vminps ymm0, ymm1, [rbx] — C5 F4 5D 03
    check_native_parity({0xC5, 0xF4, 0x5D, 0x03},
        [](CpuState& cpu) {
            cpu.gpr.set64(RBX, kAddr);
            for (int i = 0; i < 4; ++i) {
                cpu.sse.xmm[1].f32_[i] = -1.0f + static_cast<float>(i);
                cpu.avx.ymm_high[1].f32_[i] = 2.0f - static_cast<float>(i);
            }
            poison_high(cpu, 0);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

// (h) Negative controls: these VEX.256 forms are NOT element-wise and must
// stay delegated to the interpreter bridge. plain check_parity (not
// check_native_parity) because declining is the correct contract. If a future
// patch allowlists VPSLLD, its 128-bit count operand would be read from the
// high lane on pass 2 and this test fails on the result, loudly.
TEST_F(JitParity64Test, Vpslld256StaysDelegatedAndCorrect) {
    // vpslld ymm0, ymm1, xmm2 — C5 F5 F2 C2
    check_parity({0xC5, 0xF5, 0xF2, 0xC2},
        [](CpuState& cpu) {
            seed_ymm(cpu, 1, 0x00000001u, 0x80000001u);
            cpu.sse.xmm[2].u64_[0] = 3;
            cpu.sse.xmm[2].u64_[1] = 0;
            // A garbage "count" in the high lane: if the lane split ever
            // claimed this opcode it would shift the high lane by 0x7F.
            cpu.avx.ymm_high[2].u64_[0] = 0x7F;
            poison_high(cpu, 0);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}


// ============================================================================
// VEX.256 lane split — CLASS B (ADDPS/SUBPS/MULPS/DIVPS via
// EMIT_SSE_NEON_RR_FINITE_FP, and CMPPS)
//
// Class B is the FP half of the lane split, and its whole difficulty is the
// macro's RUNTIME cold arm: when any lane of either operand is Inf/NaN (or a
// zero divisor for DIV) the lowering abandons the NEON result and calls
// jit_sse_op, which computes `xmm[dst] := xmm[dst] OP xmm[src]` from register
// INDICES and is therefore structurally lane-blind. On the high-lane pass the
// emitter swaps xmm[]<->ymm_high[] around that call.
//
// The tests below deliberately drive the low and high lanes down DIFFERENT
// arms — finite low / non-finite high and vice versa — because that is the
// only shape a naive lane split gets wrong, and because a test that puts the
// same class of value in both lanes passes with the swap code deleted.
// ============================================================================

namespace {
constexpr u32 kQNanNegPayload = 0xFFC12345u;  // -qNaN, payload 0x412345
constexpr u32 kQNanPosPayload = 0x7FC0BEEFu;  // +qNaN, payload 0x40BEEF
constexpr u32 kSNan           = 0x7F812345u;  // signalling NaN
constexpr u32 kPosInf         = 0x7F800000u;
constexpr u32 kNegInf         = 0xFF800000u;
constexpr u32 kDenormMin      = 0x00000001u;  // +smallest subnormal
constexpr u32 kDenormNegMax   = 0x807FFFFFu;  // -largest subnormal
constexpr u32 kNegZero        = 0x80000000u;
constexpr u32 kPosZero        = 0x00000000u;

void seed_ps_bits(CpuState& cpu, int reg, const u32 (&lo)[4],
                  const u32 (&hi)[4]) {
    for (int i = 0; i < 4; ++i) {
        cpu.sse.xmm[reg].u32_[i] = lo[i];
        cpu.avx.ymm_high[reg].u32_[i] = hi[i];
    }
}

// Four ordinary finite values, asymmetric so a swapped operand order shows.
const u32 kFinA[4] = {0x3FC00000u /* 1.5 */, 0xC0100000u /* -2.25 */,
                      0x40700000u /* 3.75 */, 0xBF000000u /* -0.5 */};
const u32 kFinB[4] = {0x40200000u /* 2.5 */, 0x3E800000u /* 0.25 */,
                      0xC0E00000u /* -7.0 */, 0x41100000u /* 9.0 */};
// Non-finite mix: every NaN class, both infinities, and a signed zero.
const u32 kSpecial[4] = {kQNanNegPayload, kPosInf, kSNan, kNegInf};

struct FpCase { const char* name; std::initializer_list<u8> code; };
const FpCase kClassBFpOps[] = {
    {"vaddps ymm0,ymm1,ymm2", {0xC5, 0xF4, 0x58, 0xC2}},
    {"vsubps ymm0,ymm1,ymm2", {0xC5, 0xF4, 0x5C, 0xC2}},
    {"vmulps ymm0,ymm1,ymm2", {0xC5, 0xF4, 0x59, 0xC2}},
    {"vdivps ymm0,ymm1,ymm2", {0xC5, 0xF4, 0x5E, 0xC2}},
};
}  // namespace

// Both lanes finite: the pure NEON fast path, twice.
TEST_F(JitParity64Test, Vex256ClassBFpBothLanesFinite) {
    for (const auto& c : kClassBFpOps) {
        SCOPED_TRACE(c.name);
        check_native_parity(c.code,
            [](CpuState& cpu) {
                seed_ps_bits(cpu, 1, kFinA, kFinB);
                seed_ps_bits(cpu, 2, kFinB, kFinA);
                poison_high(cpu, 0);
            },
            /*check_flags=*/false, /*check_xmm=*/true);
    }
}

// LOW lane non-finite, HIGH lane finite: lane 0 takes the helper, lane 1 takes
// NEON. Catches a low-lane merge (`xmm[dst] := xmm[SRC1]`) that never happens,
// which would make the helper compute `dst OP src` with a stale destination.
TEST_F(JitParity64Test, Vex256ClassBFpNonFiniteLowLaneOnly) {
    for (const auto& c : kClassBFpOps) {
        SCOPED_TRACE(c.name);
        check_native_parity(c.code,
            [](CpuState& cpu) {
                seed_ps_bits(cpu, 1, kSpecial, kFinA);
                seed_ps_bits(cpu, 2, kFinB, kFinB);
                poison_high(cpu, 0);
            },
            /*check_flags=*/false, /*check_xmm=*/true);
    }
}

// HIGH lane non-finite, LOW lane finite. THIS is the case the physical lane
// swap exists for: the helper would otherwise compute on xmm[] — i.e. on the
// low lane's operands — and store the answer into the low half, destroying
// lane 0's already-committed result and leaving YMM[255:128] stale.
TEST_F(JitParity64Test, Vex256ClassBFpNonFiniteHighLaneOnly) {
    for (const auto& c : kClassBFpOps) {
        SCOPED_TRACE(c.name);
        check_native_parity(c.code,
            [](CpuState& cpu) {
                seed_ps_bits(cpu, 1, kFinA, kSpecial);
                seed_ps_bits(cpu, 2, kFinB, kFinB);
                poison_high(cpu, 0);
            },
            /*check_flags=*/false, /*check_xmm=*/true);
    }
}

// The mirror: the non-finite value in the r/m operand's high lane only, so the
// SECOND operand is the one that has to be swapped into place.
TEST_F(JitParity64Test, Vex256ClassBFpNonFiniteHighLaneOfRmOnly) {
    for (const auto& c : kClassBFpOps) {
        SCOPED_TRACE(c.name);
        check_native_parity(c.code,
            [](CpuState& cpu) {
                seed_ps_bits(cpu, 1, kFinA, kFinB);
                seed_ps_bits(cpu, 2, kFinB, kSpecial);
                poison_high(cpu, 0);
            },
            /*check_flags=*/false, /*check_xmm=*/true);
    }
}

TEST_F(JitParity64Test, Vex256ClassBFpNonFiniteBothLanes) {
    for (const auto& c : kClassBFpOps) {
        SCOPED_TRACE(c.name);
        check_native_parity(c.code,
            [](CpuState& cpu) {
                seed_ps_bits(cpu, 1, kSpecial, kSpecial);
                seed_ps_bits(cpu, 2, kSpecial, kFinA);
                poison_high(cpu, 0);
            },
            /*check_flags=*/false, /*check_xmm=*/true);
    }
}

// NaN SIGN and PAYLOAD, per lane. Recorded finding: ARM negated-FMA ops invert
// a propagated NaN's sign where x86 preserves it, so a NaN answer must be
// compared bit-for-bit, not just "is a NaN". check_xmm does exactly that.
TEST_F(JitParity64Test, Vex256ClassBFpNanSignAndPayloadPerLane) {
    static const u32 kNanLo[4] = {kQNanNegPayload, kQNanPosPayload, kSNan,
                                  0xFFFFFFFFu};
    static const u32 kNanHi[4] = {kQNanPosPayload, kQNanNegPayload, 0x7FFFFFFFu,
                                  kSNan};
    for (const auto& c : kClassBFpOps) {
        SCOPED_TRACE(c.name);
        // NaN in SRC1 only…
        check_native_parity(c.code,
            [](CpuState& cpu) {
                seed_ps_bits(cpu, 1, kNanLo, kNanHi);
                seed_ps_bits(cpu, 2, kFinA, kFinB);
                poison_high(cpu, 0);
            },
            /*check_flags=*/false, /*check_xmm=*/true);
        // …and in SRC2 only. Intel SDM Vol. 1 §4.8.3.4 quiets the DESTINATION
        // operand first, so these two must produce different NaNs.
        check_native_parity(c.code,
            [](CpuState& cpu) {
                seed_ps_bits(cpu, 1, kFinA, kFinB);
                seed_ps_bits(cpu, 2, kNanLo, kNanHi);
                poison_high(cpu, 0);
            },
            /*check_flags=*/false, /*check_xmm=*/true);
    }
}

// Denormals and signed zero, arranged so the two lanes differ.
TEST_F(JitParity64Test, Vex256ClassBFpDenormalsAndSignedZero) {
    static const u32 kZLo[4] = {kPosZero, kNegZero, kDenormMin, kDenormNegMax};
    static const u32 kZHi[4] = {kNegZero, kPosZero, kDenormNegMax, kDenormMin};
    for (const auto& c : kClassBFpOps) {
        SCOPED_TRACE(c.name);
        check_native_parity(c.code,
            [](CpuState& cpu) {
                seed_ps_bits(cpu, 1, kZLo, kZHi);
                seed_ps_bits(cpu, 2, kZHi, kZLo);
                poison_high(cpu, 0);
            },
            /*check_flags=*/false, /*check_xmm=*/true);
        // Signed zero as an ADDEND/SUBTRAHEND of a finite value: (-0)+(+0)
        // is +0 under round-to-nearest, and x - x is +0, not -0.
        check_native_parity(c.code,
            [](CpuState& cpu) {
                seed_ps_bits(cpu, 1, kZLo, kFinA);
                seed_ps_bits(cpu, 2, kFinA, kZHi);
                poison_high(cpu, 0);
            },
            /*check_flags=*/false, /*check_xmm=*/true);
    }
}

// DIVPS: ±0 divisors and 0/0, split across the lanes. A zero divisor forces
// the cold arm even though nothing is Inf or NaN, so this exercises the swap
// on an all-finite operand pair.
TEST_F(JitParity64Test, Vdivps256ZeroDivisorHighLaneOnly) {
    static const u32 kNum[4] = {0x3F800000u /* 1 */, 0xBF800000u /* -1 */,
                                kPosZero, kNegZero};
    static const u32 kDenZero[4] = {kPosZero, kNegZero, kPosZero, kNegZero};
    // vdivps ymm0, ymm1, ymm2 — C5 F4 5E C2
    check_native_parity({0xC5, 0xF4, 0x5E, 0xC2},
        [](CpuState& cpu) {
            seed_ps_bits(cpu, 1, kFinA, kNum);
            seed_ps_bits(cpu, 2, kFinB, kDenZero);
            poison_high(cpu, 0);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParity64Test, Vdivps256ZeroDivisorLowLaneOnly) {
    static const u32 kNum[4] = {0x3F800000u, 0xBF800000u, kPosZero, kNegZero};
    static const u32 kDenZero[4] = {kPosZero, kNegZero, kPosZero, kNegZero};
    check_native_parity({0xC5, 0xF4, 0x5E, 0xC2},
        [](CpuState& cpu) {
            seed_ps_bits(cpu, 1, kNum, kFinA);
            seed_ps_bits(cpu, 2, kDenZero, kFinB);
            poison_high(cpu, 0);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

// Inf/Inf and Inf/finite across the lanes.
TEST_F(JitParity64Test, Vdivps256InfinityMixedLanes) {
    static const u32 kInfs[4] = {kPosInf, kNegInf, kPosInf, kNegInf};
    check_native_parity({0xC5, 0xF4, 0x5E, 0xC2},
        [](CpuState& cpu) {
            seed_ps_bits(cpu, 1, kFinA, kInfs);
            seed_ps_bits(cpu, 2, kInfs, kInfs);
            poison_high(cpu, 0);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

// dst == vvvv (SRC1). On the high lane the cold arm's fused merge/swap degrades
// to a plain swap of xmm[dst] and ymm_high[dst]; the parked lane-0 result must
// still come back to the low half.
TEST_F(JitParity64Test, Vaddps256DstAliasesVvvvNonFiniteHighLane) {
    // vaddps ymm0, ymm0, ymm1 — C5 FC 58 C1
    check_native_parity({0xC5, 0xFC, 0x58, 0xC1},
        [](CpuState& cpu) {
            seed_ps_bits(cpu, 0, kFinA, kSpecial);
            seed_ps_bits(cpu, 1, kFinB, kFinB);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParity64Test, Vaddps256DstAliasesVvvvAllFinite) {
    check_native_parity({0xC5, 0xFC, 0x58, 0xC1},
        [](CpuState& cpu) {
            seed_ps_bits(cpu, 0, kFinA, kFinB);
            seed_ps_bits(cpu, 1, kFinB, kFinA);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

// r/m source == vvvv: both helper operands come from the same register, so the
// high-lane swap must not double-swap it.
TEST_F(JitParity64Test, Vaddps256RmAliasesVvvvNonFiniteHighLane) {
    // vaddps ymm0, ymm1, ymm1 — C5 F4 58 C1
    check_native_parity({0xC5, 0xF4, 0x58, 0xC1},
        [](CpuState& cpu) {
            seed_ps_bits(cpu, 1, kFinA, kSpecial);
            poison_high(cpu, 0);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

// dst == r/m source. The cold arm would stage SRC1 into xmm[dst] and thereby
// destroy the r/m source (the recorded `vex_merge_clobbers_rm_source` shape),
// and the arm is taken at RUNTIME, so the driver DECLINES this form outright.
// Plain check_parity: asserting native compilation would encode the wrong
// contract. If a later patch teaches the cold arm a scratch register, this
// test keeps passing and only the emitted-count assertions change.
// The seedings below are chosen so a `SRC1 OP SRC1` bug actually CHANGES the
// answer. The obvious seeding does not: if the non-finite value sits in SRC1,
// then NaN+NaN and NaN+finite quiet to the same NaN and Inf+Inf and
// Inf+finite are both Inf, so the operand that got clobbered is invisible.
// SRC1 is therefore kept finite and the special values put in the r/m
// operand, which is the one the clobber would destroy.
TEST_F(JitParity64Test, Vaddps256DstAliasesRmSourceStaysDelegated) {
    // vaddps ymm2, ymm1, ymm2 — C5 F4 58 D2. High lane goes down the cold arm.
    check_parity({0xC5, 0xF4, 0x58, 0xD2},
        [](CpuState& cpu) {
            seed_ps_bits(cpu, 1, kFinA, kFinA);
            seed_ps_bits(cpu, 2, kFinB, kSpecial);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
    // …and with the cold arm on the LOW lane instead.
    check_parity({0xC5, 0xF4, 0x58, 0xD2},
        [](CpuState& cpu) {
            seed_ps_bits(cpu, 1, kFinA, kFinA);
            seed_ps_bits(cpu, 2, kSpecial, kFinB);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParity64Test, Vdivps256DstAliasesRmSourceStaysDelegated) {
    static const u32 kDenZero[4] = {kPosZero, kNegZero, kPosZero, kNegZero};
    // vdivps ymm2, ymm1, ymm2 — C5 F4 5E D2. A zero divisor in the r/m
    // operand's high lane forces the cold arm on finite data.
    check_parity({0xC5, 0xF4, 0x5E, 0xD2},
        [](CpuState& cpu) {
            seed_ps_bits(cpu, 1, kFinA, kFinA);
            seed_ps_bits(cpu, 2, kFinB, kDenZero);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
    check_parity({0xC5, 0xF4, 0x5E, 0xD2},
        [](CpuState& cpu) {
            seed_ps_bits(cpu, 1, kFinA, kFinA);
            seed_ps_bits(cpu, 2, kDenZero, kFinB);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

// ---- Memory operand, above 4 GiB, with the cold arm on the HIGH lane. ----
// The cold arm RECOMPUTES the EA, so it must see the driver's +16 on the
// displacement; a lane-blind recompute would re-read the LOW 16 bytes.
TEST_F(JitParity64Test, Vaddps256MemAbove4GiBNonFiniteHighLaneOnly) {
    constexpr u64 kHigh = 0x1'0000'4000ull;
    map_tracked_region(kHigh, 0x1000);
    // Low 16 bytes finite, high 16 bytes Inf/NaN.
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(m_memory->write_u32(kHigh + 4 * i, kFinB[i]));
        ASSERT_TRUE(m_memory->write_u32(kHigh + 16 + 4 * i, kSpecial[i]));
    }
    // vaddps ymm0, ymm1, [rbx] — C5 F4 58 03
    check_native_parity({0xC5, 0xF4, 0x58, 0x03},
        [](CpuState& cpu) {
            cpu.gpr.set64(RBX, kHigh);
            seed_ps_bits(cpu, 1, kFinA, kFinA);
            poison_high(cpu, 0);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParity64Test, Vaddps256MemAbove4GiBNonFiniteLowLaneOnly) {
    constexpr u64 kHigh = 0x1'0000'5000ull;
    map_tracked_region(kHigh, 0x1000);
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(m_memory->write_u32(kHigh + 4 * i, kSpecial[i]));
        ASSERT_TRUE(m_memory->write_u32(kHigh + 16 + 4 * i, kFinB[i]));
    }
    check_native_parity({0xC5, 0xF4, 0x58, 0x03},
        [](CpuState& cpu) {
            cpu.gpr.set64(RBX, kHigh);
            seed_ps_bits(cpu, 1, kFinA, kFinA);
            poison_high(cpu, 0);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

// …and with the +16 crossing a 32-bit boundary, on the cold arm.
TEST_F(JitParity64Test, Vdivps256MemHighLaneCrosses32BitBoundaryZeroDivisor) {
    constexpr u64 kRegion = 0x1'FFFF'F000ull;
    constexpr u64 kAddr   = 0x1'FFFF'FFF0ull;   // +16 == 0x2'0000'0000
    map_tracked_region(kRegion, 0x2000);
    static const u32 kDenZero[4] = {kPosZero, kNegZero, kPosZero, kNegZero};
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(m_memory->write_u32(kAddr + 4 * i, kFinB[i]));
        ASSERT_TRUE(m_memory->write_u32(kAddr + 16 + 4 * i, kDenZero[i]));
    }
    // vdivps ymm0, ymm1, [rbx] — C5 F4 5E 03
    check_native_parity({0xC5, 0xF4, 0x5E, 0x03},
        [](CpuState& cpu) {
            cpu.gpr.set64(RBX, kAddr);
            seed_ps_bits(cpu, 1, kFinA, kFinA);
            poison_high(cpu, 0);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

// ---- VEX.128 regression guards: the 128-bit form must STILL zero the high
// lane. The lane-split driver must not steal it, and the macro's new
// vvvv-aware SRC1 read must not disturb the bridge's merge copy. ----
// NOTE ON TEST SIZE: JitCompiler demotes a guest address to the interpreter
// after kChurnRecompileLimit (8) recompiles of the same start_addr
// (jit_compiler.cpp:2891), and every check_parity call in this fixture
// compiles at 0x1000. A TEST_F that drives MORE than 8 compiles therefore
// starts returning a null CompiledBlock, which check_native_parity reports as
// "Expected a fully native 64-bit JIT block" — a harness artifact that looks
// exactly like a declined lowering. Keep each test at 8 compiles or fewer.
TEST_F(JitParity64Test, Vex128ClassBArithStillZeroesHighLane) {
    struct Case { const char* name; std::initializer_list<u8> code; };
    const Case cases[] = {
        {"vaddps xmm0,xmm1,xmm2", {0xC5, 0xF0, 0x58, 0xC2}},
        {"vsubps xmm0,xmm1,xmm2", {0xC5, 0xF0, 0x5C, 0xC2}},
        {"vmulps xmm0,xmm1,xmm2", {0xC5, 0xF0, 0x59, 0xC2}},
        {"vdivps xmm0,xmm1,xmm2", {0xC5, 0xF0, 0x5E, 0xC2}},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        // Finite operands: the NEON fast path, with the bridge's high-lane
        // zero in front of it.
        check_native_parity(c.code,
            [](CpuState& cpu) {
                seed_ps_bits(cpu, 1, kFinA, kSpecial);
                seed_ps_bits(cpu, 2, kFinB, kSpecial);
                poison_high(cpu, 0);
            },
            /*check_flags=*/false, /*check_xmm=*/true);
        // …and down the cold arm, where the helper writes xmm[] directly and
        // must not disturb the zeroed upper half.
        check_native_parity(c.code,
            [](CpuState& cpu) {
                seed_ps_bits(cpu, 1, kSpecial, kFinA);
                seed_ps_bits(cpu, 2, kFinB, kFinB);
                poison_high(cpu, 0);
            },
            /*check_flags=*/false, /*check_xmm=*/true);
    }
}

TEST_F(JitParity64Test, Vcmpps128StillZeroesHighLane) {
    // vcmpltps xmm0, xmm1, xmm2 — C5 F0 C2 C2 01
    check_native_parity({0xC5, 0xF0, 0xC2, 0xC2, 0x01},
        [](CpuState& cpu) {
            seed_ps_bits(cpu, 1, kFinA, kSpecial);
            seed_ps_bits(cpu, 2, kFinB, kSpecial);
            poison_high(cpu, 0);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
    // vcmpunordps xmm0, xmm1, xmm2 — C5 F0 C2 C2 03
    check_native_parity({0xC5, 0xF0, 0xC2, 0xC2, 0x03},
        [](CpuState& cpu) {
            seed_ps_bits(cpu, 1, kSpecial, kFinA);
            seed_ps_bits(cpu, 2, kFinB, kFinB);
            poison_high(cpu, 0);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

// ---- CMPPS. Unlike the arithmetic forms it has NO helper arm at all, so the
// lane split is a pure re-emission; the only change it needed was reading SRC1
// from vvvv instead of the destination. All 8 legacy predicates, with NaN in
// exactly one lane so the ordered/unordered halves of each predicate are
// exercised independently per lane. ----
// Split in two: 8 predicates x 2 seedings would be 16 compiles at one guest
// address and would trip the churn guard (see the note above).
TEST_F(JitParity64Test, Vcmpps256PredicatesZeroToThreeNanInOneLane) {
    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    for (u8 pred = 0; pred < 4; ++pred) {
        SCOPED_TRACE(static_cast<int>(pred));
        const std::vector<u8> code{0xC5, 0xF4, 0xC2, 0xC2, pred};
        // NaN in the HIGH lane only.
        check_parity(code.data(), code.size(),
            [](CpuState& cpu) {
                seed_ps_bits(cpu, 1, kFinA, kSpecial);
                seed_ps_bits(cpu, 2, kFinB, kFinB);
                poison_high(cpu, 0);
            },
            /*check_flags=*/false, /*check_xmm=*/true, /*require_jit=*/true);
        // NaN in the LOW lane only.
        check_parity(code.data(), code.size(),
            [](CpuState& cpu) {
                seed_ps_bits(cpu, 1, kSpecial, kFinA);
                seed_ps_bits(cpu, 2, kFinB, kFinB);
                poison_high(cpu, 0);
            },
            /*check_flags=*/false, /*check_xmm=*/true, /*require_jit=*/true);
    }
}

TEST_F(JitParity64Test, Vcmpps256PredicatesFourToSevenNanInOneLane) {
    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    for (u8 pred = 4; pred < 8; ++pred) {
        SCOPED_TRACE(static_cast<int>(pred));
        const std::vector<u8> code{0xC5, 0xF4, 0xC2, 0xC2, pred};
        check_parity(code.data(), code.size(),
            [](CpuState& cpu) {
                seed_ps_bits(cpu, 1, kFinA, kSpecial);
                seed_ps_bits(cpu, 2, kFinB, kFinB);
                poison_high(cpu, 0);
            },
            /*check_flags=*/false, /*check_xmm=*/true, /*require_jit=*/true);
        check_parity(code.data(), code.size(),
            [](CpuState& cpu) {
                seed_ps_bits(cpu, 1, kSpecial, kFinA);
                seed_ps_bits(cpu, 2, kFinB, kFinB);
                poison_high(cpu, 0);
            },
            /*check_flags=*/false, /*check_xmm=*/true, /*require_jit=*/true);
    }
}

// CMPPS has no merge copy and loads both sources before storing the
// destination, so dst == r/m source IS lowered natively here (unlike the
// arithmetic forms, which decline it).
TEST_F(JitParity64Test, Vcmpps256DstAliasesRmSource) {
    // vcmpeqps ymm2, ymm1, ymm2 — C5 F4 C2 D2 00. SRC1 and the r/m operand
    // must DIFFER, or `SRC1 CMP SRC1` (all-ones for EQ) is indistinguishable
    // from the right answer.
    check_native_parity({0xC5, 0xF4, 0xC2, 0xD2, 0x00},
        [](CpuState& cpu) {
            seed_ps_bits(cpu, 1, kFinA, kFinA);
            seed_ps_bits(cpu, 2, kFinB, kSpecial);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
    // vcmpneqps ymm2, ymm1, ymm2 — C5 F4 C2 D2 04
    check_native_parity({0xC5, 0xF4, 0xC2, 0xD2, 0x04},
        [](CpuState& cpu) {
            seed_ps_bits(cpu, 1, kFinA, kSpecial);
            seed_ps_bits(cpu, 2, kFinB, kFinB);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParity64Test, Vcmpps256DstAliasesVvvv) {
    // vcmpltps ymm0, ymm0, ymm1 — C5 FC C2 C1 01
    check_native_parity({0xC5, 0xFC, 0xC2, 0xC1, 0x01},
        [](CpuState& cpu) {
            seed_ps_bits(cpu, 0, kFinA, kSpecial);
            seed_ps_bits(cpu, 1, kFinB, kFinB);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}
