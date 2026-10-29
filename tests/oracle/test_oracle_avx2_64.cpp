// SPDX-License-Identifier: LGPL-2.1-only
// x86-64 interpreter/JIT ↔ Oracle AVX2 unique integer tests (VEX.128).
//
// Same harness as test_oracle_avx_64.cpp. Fixture is OracleAvx2_64 so
// this TU does not ODR-collide with OracleAvx64 / OracleInteger64.
//
// Intel SDM Vol. 2 VPSRLVD / VPBLENDD: DEST := SRC1 OP SRC2; flags unaffected.

#include "oracle/oracle_client.hpp"
#include "oracle/oracle_contract.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

using vitreo::oracle::State;
using vitreo::oracle::contract::compare;
using vitreo::oracle::contract::CompareOpts;
using vitreo::oracle::contract::run_on_interpreter_64;
using vitreo::oracle::contract::run_on_jit_64;

class OracleAvx2_64 : public ::testing::Test {
protected:
    void SetUp() override {
        if (!vitreo::oracle::available()) {
            GTEST_SKIP() << "oracle runner not available (requires the host x86 runner)";
        }
    }

    // Deterministic, non-trivial 64-bit seed state. Values stay under 2^48 so
    // no register can form a non-canonical address if used as a base.
    static State seed_state() {
        State s{};
        s.mode = vitreo::oracle::kMode64;
        s.rflags = 0x202;
        s.mxcsr = 0x1F80;
        s.rip = vitreo::oracle::kCodeVa64;
        s.mem_va = vitreo::oracle::kMemVa;
        static const uint64_t kSeeds[16] = {
            0x0000'1111'2222'3333ull, 0x0000'4444'5555'6666ull,
            0x0000'7777'8888'9999ull, 0x0000'AAAA'BBBB'CCCCull,
            0,  // RSP: not loaded by either side
            0x0000'0000'0000'0007ull,
            0x0000'DEAD'BEEF'0BADull, vitreo::oracle::kMemVa,
            0x0000'0102'0304'0506ull, 0x0000'0F0E'0D0C'0B0Aull,
            0x0000'0000'FFFF'FFFFull, 0x0000'8000'0000'0001ull,
            0x0000'1234'5678'9ABCull, 0x0000'0000'0000'0040ull,
            0x0000'7FFF'FFFF'FFFFull, 0x0000'0000'0000'0001ull,
        };
        for (int i = 0; i < 16; ++i) s.gpr[i] = kSeeds[i];
        for (int r = 0; r < 16; ++r) {
            for (int b = 0; b < 16; ++b) {
                s.xmm[r][b] = static_cast<uint8_t>(0x10 * r + b);
            }
        }
        for (size_t b = 0; b < s.mem.size(); ++b) {
            s.mem[b] = static_cast<uint8_t>(0xA0 + b);
        }
        return s;
    }

    static void run_triplet(const State& in, const std::vector<uint8_t>& code,
                            CompareOpts opts = {}) {
        State oracle_out{};
        ASSERT_TRUE(vitreo::oracle::execute(in, code, &oracle_out))
            << "oracle execution failed";
        ASSERT_EQ(oracle_out.status, 0u) << "oracle snippet faulted";
        ASSERT_EQ(oracle_out.rip, vitreo::oracle::kCodeVa64)
            << "oracle did not place the snippet at the requested VA";

        State interp_out = run_on_interpreter_64(in, code);
        ASSERT_EQ(interp_out.status, 0u) << "interpreter snippet faulted";
        std::string interp_diff = compare(interp_out, oracle_out, opts);
        EXPECT_TRUE(interp_diff.empty())
            << "interpreter ↔ oracle divergence:\n" << interp_diff;

        State jit_out = run_on_jit_64(in, code);
        if (jit_out.status == 7u) {
            GTEST_SKIP() << "JIT has no native lowering for this snippet";
        }
        ASSERT_EQ(jit_out.status, 0u) << "JIT snippet faulted";
        std::string jit_diff = compare(jit_out, oracle_out, opts);
        EXPECT_TRUE(jit_diff.empty())
            << "JIT ↔ oracle divergence:\n" << jit_diff;
    }
};

// VPSRLVD xmm0, xmm1, xmm1 — C4 E2 71 45 C1. Per-dword logical right shift.
TEST_F(OracleAvx2_64, VpsrlvdXmm0Xmm1Xmm1) {
    State s = seed_state();
    s.rflags = 0x202 | vitreo::oracle::contract::kArithFlagsMask;
    run_triplet(s, {0xC4, 0xE2, 0x71, 0x45, 0xC1});
}

// VPBLENDD xmm0, xmm1, xmm1, 0xAA — C4 E3 71 02 C1 AA. Dword blend; flags unchanged.
TEST_F(OracleAvx2_64, VpblenddXmm0Xmm1Xmm1Aa) {
    State s = seed_state();
    s.rflags = 0x202 | vitreo::oracle::contract::kArithFlagsMask;
    run_triplet(s, {0xC4, 0xE3, 0x71, 0x02, 0xC1, 0xAA});
}

// VBROADCASTSS xmm0, xmm1 — C4 E2 79 18 C1. Finite SS in xmm1[0].
TEST_F(OracleAvx2_64, VbroadcastssXmm0Xmm1) {
    State s = seed_state();
    const uint32_t finite = 0x3F800001u;
    std::memcpy(s.xmm[1].data(), &finite, 4);
    run_triplet(s, {0xC4, 0xE2, 0x79, 0x18, 0xC1});
}

TEST_F(OracleAvx2_64, VbroadcastssYmm0Xmm1) {
    State s = seed_state();
    const uint32_t finite = 0x3F800001u;
    std::memcpy(s.xmm[1].data(), &finite, 4);
    run_triplet(s, {0xC4, 0xE2, 0x7D, 0x18, 0xC1});
}

TEST_F(OracleAvx2_64, VbroadcastsdYmm0Xmm1) {
    State s = seed_state();
    const uint64_t finite = 0x3FF0'0000'0000'0001ull;
    std::memcpy(s.xmm[1].data(), &finite, 8);
    run_triplet(s, {0xC4, 0xE2, 0x7D, 0x19, 0xC1});
}

TEST_F(OracleAvx2_64, VpbroadcastdYmm0Xmm1) {
    run_triplet(seed_state(), {0xC4, 0xE2, 0x7D, 0x58, 0xC1});
}

TEST_F(OracleAvx2_64, VpermdYmm0Ymm1Ymm1) {
    run_triplet(seed_state(), {0xC4, 0xE2, 0x75, 0x36, 0xC1});
}

TEST_F(OracleAvx2_64, VpermqYmm0Ymm1E4) {
    run_triplet(seed_state(), {0xC4, 0xE3, 0xFD, 0x00, 0xC1, 0xE4});
}

// VPMASKMOVD xmm0, xmm1, [rdi] — C4 E2 71 8C 07. Mixed dword mask.
TEST_F(OracleAvx2_64, VpmaskmovdLoadXmm0Xmm1Rdi) {
    State s = seed_state();
    for (int i = 0; i < 4; ++i) {
        const uint32_t m = (i & 1) ? 0x80000000u : 0u;
        std::memcpy(s.xmm[1].data() + static_cast<size_t>(i) * 4, &m, 4);
    }
    run_triplet(s, {0xC4, 0xE2, 0x71, 0x8C, 0x07});
}

TEST_F(OracleAvx2_64, VpmaskmovdStoreRdiXmm1Xmm0) {
    State s = seed_state();
    for (int i = 0; i < 4; ++i) {
        const uint32_t m = (i & 1) ? 0x80000000u : 0u;
        std::memcpy(s.xmm[1].data() + static_cast<size_t>(i) * 4, &m, 4);
    }
    run_triplet(s, {0xC4, 0xE2, 0x71, 0x8E, 0x07});
}

TEST_F(OracleAvx2_64, VpmaskmovqLoadXmm0Xmm1Rdi) {
    State s = seed_state();
    const uint64_t lo = 0x8000'0000'0000'0000ull;
    const uint64_t hi = 0;
    std::memcpy(s.xmm[1].data(), &lo, 8);
    std::memcpy(s.xmm[1].data() + 8, &hi, 8);
    run_triplet(s, {0xC4, 0xE2, 0xF1, 0x8C, 0x07});
}

TEST_F(OracleAvx2_64, VpmaskmovqStoreRdiXmm1Xmm0) {
    State s = seed_state();
    const uint64_t lo = 0x8000'0000'0000'0000ull;
    const uint64_t hi = 0;
    std::memcpy(s.xmm[1].data(), &lo, 8);
    std::memcpy(s.xmm[1].data() + 8, &hi, 8);
    run_triplet(s, {0xC4, 0xE2, 0xF1, 0x8E, 0x07});
}

// VEXTRACTI128 xmm0, ymm1, 0 — C4 E3 7D 39 C8 00. Low 128 of ymm1.
TEST_F(OracleAvx2_64, Vextracti128Xmm0Ymm1Imm0) {
    run_triplet(seed_state(), {0xC4, 0xE3, 0x7D, 0x39, 0xC8, 0x00});
}

// VINSERTI128 ymm0, ymm1, xmm1, 0 — C4 E3 75 38 C1 00. Dest.low := xmm1.
TEST_F(OracleAvx2_64, Vinserti128Ymm0Ymm1Xmm1Imm0) {
    run_triplet(seed_state(), {0xC4, 0xE3, 0x75, 0x38, 0xC1, 0x00});
}

// VPERM2I128 ymm0, ymm1, ymm1, 0x00 — dest.low from src1.low.
TEST_F(OracleAvx2_64, Vperm2i128Ymm0Ymm1Ymm1Imm00) {
    run_triplet(seed_state(), {0xC4, 0xE3, 0x75, 0x46, 0xC1, 0x00});
}

// VPERMPS ymm0, ymm1, ymm1 — C4 E2 75 16 C1. Low 128 from src[idx & 7].
TEST_F(OracleAvx2_64, VpermpsYmm0Ymm1Ymm1) {
    run_triplet(seed_state(), {0xC4, 0xE2, 0x75, 0x16, 0xC1});
}

// VPERMPD ymm0, ymm1, 0xE4 — C4 E3 FD 01 C1 E4. Identity on dest.low.
TEST_F(OracleAvx2_64, VpermpdYmm0Ymm1E4) {
    run_triplet(seed_state(), {0xC4, 0xE3, 0xFD, 0x01, 0xC1, 0xE4});
}

// VBROADCASTF128 ymm0, [rdi] — C4 E2 7D 1A 07. Low 128 copies [rdi].
TEST_F(OracleAvx2_64, Vbroadcastf128Ymm0Rdi) {
    run_triplet(seed_state(), {0xC4, 0xE2, 0x7D, 0x1A, 0x07});
}

// VBROADCASTI128 ymm0, [rdi] — C4 E2 7D 5A 07.
TEST_F(OracleAvx2_64, Vbroadcasti128Ymm0Rdi) {
    run_triplet(seed_state(), {0xC4, 0xE2, 0x7D, 0x5A, 0x07});
}

// VGATHERDPS xmm0, [rdi+xmm2*4], xmm1 — C4 E2 71 92 04 97.
// Intel SDM Vol. 2: dest/index/mask distinct; EA = base + sext(idx)*scale.
TEST_F(OracleAvx2_64, VgatherdpsXmm0RdiXmm2Xmm1) {
    State s = seed_state();
    const uint32_t mask = 0x80000000u;
    for (int i = 0; i < 4; ++i) {
        const uint32_t idx = static_cast<uint32_t>(i);
        std::memcpy(s.xmm[1].data() + static_cast<size_t>(i) * 4, &mask, 4);
        std::memcpy(s.xmm[2].data() + static_cast<size_t>(i) * 4, &idx, 4);
    }
    run_triplet(s, {0xC4, 0xE2, 0x71, 0x92, 0x04, 0x97});
}

// VPGATHERDD xmm0, [rdi+xmm2*4], xmm1 — C4 E2 71 90 04 97.
TEST_F(OracleAvx2_64, VpgatherddXmm0RdiXmm2Xmm1) {
    State s = seed_state();
    const uint32_t mask = 0x80000000u;
    for (int i = 0; i < 4; ++i) {
        const uint32_t idx = static_cast<uint32_t>(i);
        std::memcpy(s.xmm[1].data() + static_cast<size_t>(i) * 4, &mask, 4);
        std::memcpy(s.xmm[2].data() + static_cast<size_t>(i) * 4, &idx, 4);
    }
    run_triplet(s, {0xC4, 0xE2, 0x71, 0x90, 0x04, 0x97});
}

// ---------------------------------------------------------------------------
// 256-bit VEX sweep.
//
// State carries XMM[16] only, so a YMM high lane is invisible to `compare`.
// These tests make it visible: a prologue fills ymm0..ymm4 from the shared
// scratch page (so both sides start from identical 256-bit values) and an
// epilogue lands each high lane in xmm11..xmm15 with VEXTRACTI128, which the
// oracle does compare. Without this every 256-bit form is untested — see the
// AVX2-gated wrong result that made Spotify's protobuf reject every symbol.
// ---------------------------------------------------------------------------

// mem[] pattern with a 32-byte period plus deliberate breaks, so a lane-vs-lane
// compare yields a mixed mask instead of all-ones or all-zeros, and NULs occur
// in both lanes.
State ymm_seed_state_from(State s) {
    for (size_t b = 0; b < s.mem.size(); ++b) {
        uint8_t v = static_cast<uint8_t>(b % 32);
        if ((b % 7) == 0) v = static_cast<uint8_t>(v ^ 0x5A);
        if ((b % 11) == 0) v = 0;
        s.mem[b] = v;
    }
    return s;
}

class OracleAvx2Ymm64 : public OracleAvx2_64 {
protected:
    // vmovdqu ymm0..ymm4, [rdi + 0x00/0x20/0x40/0x60/0x80]
    static void append_prologue(std::vector<uint8_t>& code) {
        const uint8_t p[] = {
            0xC5, 0xFE, 0x6F, 0x07,
            0xC5, 0xFE, 0x6F, 0x4F, 0x20,
            0xC5, 0xFE, 0x6F, 0x57, 0x40,
            0xC5, 0xFE, 0x6F, 0x5F, 0x60,
            0xC5, 0xFE, 0x6F, 0xA7, 0x80, 0x00, 0x00, 0x00,
        };
        code.insert(code.end(), std::begin(p), std::end(p));
    }

    // vextracti128 xmm11..xmm15, ymm0..ymm4, 1
    static void append_epilogue(std::vector<uint8_t>& code) {
        const uint8_t e[] = {
            0xC4, 0xC3, 0x7D, 0x39, 0xC3, 0x01,
            0xC4, 0xC3, 0x7D, 0x39, 0xCC, 0x01,
            0xC4, 0xC3, 0x7D, 0x39, 0xD5, 0x01,
            0xC4, 0xC3, 0x7D, 0x39, 0xDE, 0x01,
            0xC4, 0xC3, 0x7D, 0x39, 0xE7, 0x01,
        };
        code.insert(code.end(), std::begin(e), std::end(e));
    }

    void run_ymm(std::initializer_list<uint8_t> op) {
        std::vector<uint8_t> code;
        append_prologue(code);
        code.insert(code.end(), op.begin(), op.end());
        append_epilogue(code);
        State s = ymm_seed_state_from(seed_state());
        s.rflags = 0x202 | vitreo::oracle::contract::kArithFlagsMask;
        run_triplet(s, code);
    }
};

// The prologue/epilogue alone: VMOVDQU ymm load + VEXTRACTI128 must agree
// before any result below means anything.
TEST_F(OracleAvx2Ymm64, PrologueEpilogueRoundTrip) { run_ymm({0x90}); }

// VMOVDQA [rdi+0x80], ymm0 — C5 FD 7F 87 80 00 00 00. 32-byte store.
TEST_F(OracleAvx2Ymm64, VmovdqaStoreYmm) {
    run_ymm({0xC5, 0xFD, 0x7F, 0x87, 0x80, 0x00, 0x00, 0x00});
}

// VPBROADCASTB ymm0, xmm1 — C4 E2 7D 78 C1. All 32 bytes := SRC[7:0].
TEST_F(OracleAvx2Ymm64, VpbroadcastbYmm) {
    run_ymm({0xC4, 0xE2, 0x7D, 0x78, 0xC1});
}

// VPCMPEQB ymm1, ymm1, [rdi] — C5 F5 74 0F. The strlen/memchr kernel.
TEST_F(OracleAvx2Ymm64, VpcmpeqbYmmMem) { run_ymm({0xC5, 0xF5, 0x74, 0x0F}); }

// VPCMPEQW ymm1, ymm1, [rdi] — C5 F5 75 0F. The wcslen kernel.
TEST_F(OracleAvx2Ymm64, VpcmpeqwYmmMem) { run_ymm({0xC5, 0xF5, 0x75, 0x0F}); }

// VPMASKMOVD ymm0, ymm2, [rdi] — C4 E2 6D 8C 07. Mask is VEX.vvvv.
TEST_F(OracleAvx2Ymm64, VpmaskmovdYmmLoad) {
    run_ymm({0xC4, 0xE2, 0x6D, 0x8C, 0x07});
}

// VPMOVMSKB eax, ymm1 — C5 FD D7 C1. Collapses both lanes into a GPR.
TEST_F(OracleAvx2Ymm64, VpmovmskbYmm) { run_ymm({0xC5, 0xFD, 0xD7, 0xC1}); }

// VPSADBW ymm0, ymm4, ymm2 — C5 DD F6 C2. Per-64-bit-block byte SAD.
TEST_F(OracleAvx2Ymm64, VpsadbwYmm) { run_ymm({0xC5, 0xDD, 0xF6, 0xC2}); }

// VPSUBB ymm2, ymm2, ymm1 — C5 ED F8 D1.
TEST_F(OracleAvx2Ymm64, VpsubbYmm) { run_ymm({0xC5, 0xED, 0xF8, 0xD1}); }

// VINSERTF128 ymm0, ymm0, xmm3, 1 — C4 E3 7D 18 C3 01.
TEST_F(OracleAvx2Ymm64, Vinsertf128Ymm) {
    run_ymm({0xC4, 0xE3, 0x7D, 0x18, 0xC3, 0x01});
}

// VEXTRACTI128 xmm1, ymm0, 1 — C4 E3 7D 39 C1 01 (also clears ymm1's high).
TEST_F(OracleAvx2Ymm64, Vextracti128Ymm) {
    run_ymm({0xC4, 0xE3, 0x7D, 0x39, 0xC1, 0x01});
}

// A 128-bit VEX op must ZERO bits 255:128 of its destination (Intel SDM
// Vol. 1 14.3). The extract epilogue is what makes that observable.
TEST_F(OracleAvx2Ymm64, Vpxor128ZeroesHighLane) {
    run_ymm({0xC5, 0xF1, 0xEF, 0xC9});  // vpxor xmm1, xmm1, xmm1
}

TEST_F(OracleAvx2Ymm64, Vmovd128ZeroesHighLane) {
    run_ymm({0xC5, 0xF9, 0x6E, 0xD2});  // vmovd xmm2, edx
}

TEST_F(OracleAvx2Ymm64, Vpshufb128ZeroesHighLane) {
    run_ymm({0xC4, 0xE2, 0x61, 0x00, 0xD8});  // vpshufb xmm3, xmm3, xmm0
}

TEST_F(OracleAvx2Ymm64, Vpaddq128ZeroesHighLane) {
    run_ymm({0xC5, 0xF9, 0xD4, 0xC2});  // vpaddq xmm0, xmm0, xmm2
}

TEST_F(OracleAvx2Ymm64, Vzeroupper) { run_ymm({0xC5, 0xF8, 0x77}); }

// The 256-bit VEX logical/compare register forms the MSVC vectorized string
// routines use: `vpmaskmovd ymm0,ymm2,[mem]; vpcmpeqb ymm1,ymm0,ymm3;
// vpand ymm2,ymm1,ymm2; vpmovmskb eax,ymm2` is the dword-granular tail of
// __std_find_trivial_1 / __std_count_trivial_1.
TEST_F(OracleAvx2Ymm64, VpandYmm) { run_ymm({0xC5, 0xF5, 0xDB, 0xD2}); }
TEST_F(OracleAvx2Ymm64, VporYmm) { run_ymm({0xC5, 0xF5, 0xEB, 0xD2}); }
TEST_F(OracleAvx2Ymm64, VpxorYmm) { run_ymm({0xC5, 0xF5, 0xEF, 0xD2}); }
TEST_F(OracleAvx2Ymm64, VpandnYmm) { run_ymm({0xC5, 0xF5, 0xDF, 0xD2}); }
TEST_F(OracleAvx2Ymm64, VpcmpeqbYmmReg) { run_ymm({0xC5, 0xF5, 0x74, 0xCB}); }
TEST_F(OracleAvx2Ymm64, VpcmpeqwYmmReg) { run_ymm({0xC5, 0xF5, 0x75, 0xCB}); }
TEST_F(OracleAvx2Ymm64, VpaddqYmm) { run_ymm({0xC5, 0xF5, 0xD4, 0xD2}); }
TEST_F(OracleAvx2Ymm64, VpsubbYmmReg) { run_ymm({0xC5, 0xF5, 0xF8, 0xD2}); }

// The whole __std_find_trivial_1 dword tail, end to end.
TEST_F(OracleAvx2Ymm64, FindTrivialDwordTailKernel) {
    std::vector<uint8_t> code;
    append_prologue(code);
    const uint8_t kernel[] = {
        0xC4, 0xE2, 0x6D, 0x8C, 0x07,  // vpmaskmovd ymm0, ymm2, [rdi]
        0xC5, 0xFD, 0x74, 0xCB,        // vpcmpeqb  ymm1, ymm0, ymm3
        0xC5, 0xF5, 0xDB, 0xD2,        // vpand     ymm2, ymm1, ymm2
        0xC5, 0xFD, 0xD7, 0xC2,        // vpmovmskb eax,  ymm2
    };
    code.insert(code.end(), std::begin(kernel), std::end(kernel));
    append_epilogue(code);
    State s = ymm_seed_state_from(seed_state());
    s.rflags = 0x202 | vitreo::oracle::contract::kArithFlagsMask;
    run_triplet(s, code);
}

}  // namespace