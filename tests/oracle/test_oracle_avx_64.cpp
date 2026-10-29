// SPDX-License-Identifier: LGPL-2.1-only
// x86-64 interpreter/JIT ↔ Oracle AVX integer/bitwise tests (VEX.128).
//
// Same harness as test_oracle_integer_64.cpp. Fixture is OracleAvx64 so
// this TU does not ODR-collide with OracleInteger64 / OracleContract64.
//
// Packed FP arith is excluded (Rosetta vs Apple NaN-payload drift).
// Intel SDM Vol. 2 VPAND/VPADDD: DEST := SRC1 OP SRC2; flags unaffected.

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

class OracleAvx64 : public ::testing::Test {
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

    // F16C #UDs on hosts without CPUID.01H:ECX.F16C (possible on Rosetta).
    static void run_triplet_skip_ud(const State& in,
                                    const std::vector<uint8_t>& code) {
        State oracle_out{};
        ASSERT_TRUE(vitreo::oracle::execute(in, code, &oracle_out))
            << "oracle execution failed";
        if (oracle_out.status != 0u) {
            GTEST_SKIP() << "oracle snippet faulted (missing F16C CPUID)";
        }
        ASSERT_EQ(oracle_out.rip, vitreo::oracle::kCodeVa64)
            << "oracle did not place the snippet at the requested VA";

        State interp_out = run_on_interpreter_64(in, code);
        ASSERT_EQ(interp_out.status, 0u) << "interpreter snippet faulted";
        std::string interp_diff = compare(interp_out, oracle_out);
        EXPECT_TRUE(interp_diff.empty())
            << "interpreter ↔ oracle divergence:\n" << interp_diff;

        State jit_out = run_on_jit_64(in, code);
        if (jit_out.status == 7u) {
            GTEST_SKIP() << "JIT has no native lowering for this snippet";
        }
        ASSERT_EQ(jit_out.status, 0u) << "JIT snippet faulted";
        std::string jit_diff = compare(jit_out, oracle_out);
        EXPECT_TRUE(jit_diff.empty())
            << "JIT ↔ oracle divergence:\n" << jit_diff;
    }
};

// VPAND xmm0, xmm1, xmm1 — C5 F1 DB C1. DEST := SRC1 AND SRC2; flags unchanged.
TEST_F(OracleAvx64, VpandXmm0Xmm1Xmm1) {
    State s = seed_state();
    s.rflags = 0x202 | vitreo::oracle::contract::kArithFlagsMask;
    run_triplet(s, {0xC5, 0xF1, 0xDB, 0xC1});
}

// VPADDD xmm0, xmm1, xmm1 — C5 F1 FE C1. Packed dword add; flags unchanged.
TEST_F(OracleAvx64, VpadddXmm0Xmm1Xmm1) {
    State s = seed_state();
    s.rflags = 0x202 | vitreo::oracle::contract::kArithFlagsMask;
    run_triplet(s, {0xC5, 0xF1, 0xFE, 0xC1});
}

// VZEROUPPER — C5 F8 77. XMM lanes unchanged; flags unchanged.
TEST_F(OracleAvx64, Vzeroupper) {
    State s = seed_state();
    s.rflags = 0x202 | vitreo::oracle::contract::kArithFlagsMask;
    run_triplet(s, {0xC5, 0xF8, 0x77});
}

// VZEROALL — C5 FC 77. All XMM registers zeroed.
TEST_F(OracleAvx64, Vzeroall) {
    State s = seed_state();
    s.rflags = 0x202 | vitreo::oracle::contract::kArithFlagsMask;
    run_triplet(s, {0xC5, 0xFC, 0x77});
}

// VBROADCASTSS xmm0, [rdi] — C4 E2 79 18 07. Finite SS in scratch[0].
TEST_F(OracleAvx64, VbroadcastssXmm0Mem) {
    State s = seed_state();
    const uint32_t finite = 0x3F800001u;
    std::memcpy(s.mem.data(), &finite, 4);
    run_triplet(s, {0xC4, 0xE2, 0x79, 0x18, 0x07});
}

// VBROADCASTSS ymm0, [rdi] — C4 E2 7D 18 07. Low 128 matches the xmm form.
TEST_F(OracleAvx64, VbroadcastssYmm0Mem) {
    State s = seed_state();
    const uint32_t finite = 0x3F800001u;
    std::memcpy(s.mem.data(), &finite, 4);
    run_triplet(s, {0xC4, 0xE2, 0x7D, 0x18, 0x07});
}

// VBROADCASTSD ymm0, [rdi] — C4 E2 7D 19 07.
TEST_F(OracleAvx64, VbroadcastsdYmm0Mem) {
    State s = seed_state();
    const uint64_t finite = 0x3FF0'0000'0000'0001ull;
    std::memcpy(s.mem.data(), &finite, 8);
    run_triplet(s, {0xC4, 0xE2, 0x7D, 0x19, 0x07});
}

// VEXTRACTF128 xmm0, ymm1, 0/1 — C4 E3 7D 19 C8 ib.
TEST_F(OracleAvx64, Vextractf128Xmm0Ymm1Imm0) {
    run_triplet(seed_state(), {0xC4, 0xE3, 0x7D, 0x19, 0xC8, 0x00});
}

TEST_F(OracleAvx64, Vextractf128Xmm0Ymm1Imm1) {
    run_triplet(seed_state(), {0xC4, 0xE3, 0x7D, 0x19, 0xC8, 0x01});
}

// VINSERTF128 ymm0, ymm1, xmm1, 0/1 — C4 E3 75 18 C1 ib.
TEST_F(OracleAvx64, Vinsertf128Ymm0Ymm1Xmm1Imm0) {
    run_triplet(seed_state(), {0xC4, 0xE3, 0x75, 0x18, 0xC1, 0x00});
}

TEST_F(OracleAvx64, Vinsertf128Ymm0Ymm1Xmm1Imm1) {
    run_triplet(seed_state(), {0xC4, 0xE3, 0x75, 0x18, 0xC1, 0x01});
}

// VPERM2F128 ymm0, ymm1, ymm1, imm — dest.low from src1.low / src2.low.
TEST_F(OracleAvx64, Vperm2f128Ymm0Ymm1Ymm1Imm00) {
    run_triplet(seed_state(), {0xC4, 0xE3, 0x75, 0x06, 0xC1, 0x00});
}

TEST_F(OracleAvx64, Vperm2f128Ymm0Ymm1Ymm1Imm20) {
    run_triplet(seed_state(), {0xC4, 0xE3, 0x75, 0x06, 0xC1, 0x20});
}

// VCVTPH2PS xmm0, xmm1 — C4 E2 79 13 C1. Finite binary16 in xmm1.
TEST_F(OracleAvx64, Vcvtph2psXmm0Xmm1) {
    State s = seed_state();
    for (int i = 0; i < 8; ++i) {
        const uint16_t h = static_cast<uint16_t>(0x3C00u | (i & 0x3F));
        std::memcpy(s.xmm[1].data() + i * 2, &h, 2);
    }
    run_triplet_skip_ud(s, {0xC4, 0xE2, 0x79, 0x13, 0xC1});
}

// VCVTPS2PH xmm0, xmm1, 0 — C4 E3 79 1D C8 00. Exact binary32 so PE stays clear.
TEST_F(OracleAvx64, Vcvtps2phXmm0Xmm1Imm0) {
    State s = seed_state();
    const uint32_t exact[4] = {0x3F800000u, 0x40000000u, 0xBF800000u, 0x3F000000u};
    std::memcpy(s.xmm[1].data(), exact, sizeof(exact));
    run_triplet_skip_ud(s, {0xC4, 0xE3, 0x79, 0x1D, 0xC8, 0x00});
}

// 1 + ulp(f32) rounds to 1.0 in binary16 (truncate and RNE agree); PE is set.
TEST_F(OracleAvx64, Vcvtps2phInexactSetsPe) {
    State s = seed_state();
    const uint32_t inexact[4] = {0x3F800001u, 0x40000000u, 0xBF800000u, 0x3F000000u};
    std::memcpy(s.xmm[1].data(), inexact, sizeof(inexact));
    run_triplet_skip_ud(s, {0xC4, 0xE3, 0x79, 0x1D, 0xC8, 0x00});
}

// 1 + 1.5/1024 is halfway from an odd f16 mantissa: RNE → 0x3C02, RZ → 0x3C01.
TEST_F(OracleAvx64, Vcvtps2phRneHalfwayOdd) {
    State s = seed_state();
    const uint32_t halfway[4] = {0x3F803000u, 0x40000000u, 0xBF800000u, 0x3F000000u};
    std::memcpy(s.xmm[1].data(), halfway, sizeof(halfway));
    run_triplet_skip_ud(s, {0xC4, 0xE3, 0x79, 0x1D, 0xC8, 0x00});
}

TEST_F(OracleAvx64, Vcvtps2phTruncHalfwayOdd) {
    State s = seed_state();
    const uint32_t halfway[4] = {0x3F803000u, 0x40000000u, 0xBF800000u, 0x3F000000u};
    std::memcpy(s.xmm[1].data(), halfway, sizeof(halfway));
    run_triplet_skip_ud(s, {0xC4, 0xE3, 0x79, 0x1D, 0xC8, 0x03});
}

TEST_F(OracleAvx64, Vcvtps2phRoundDownHalfwayOdd) {
    State s = seed_state();
    const uint32_t halfway[4] = {0x3F803000u, 0x40000000u, 0xBF800000u, 0x3F000000u};
    std::memcpy(s.xmm[1].data(), halfway, sizeof(halfway));
    run_triplet_skip_ud(s, {0xC4, 0xE3, 0x79, 0x1D, 0xC8, 0x01});
}

TEST_F(OracleAvx64, Vcvtps2phRoundUpHalfwayOdd) {
    State s = seed_state();
    const uint32_t halfway[4] = {0x3F803000u, 0x40000000u, 0xBF800000u, 0x3F000000u};
    std::memcpy(s.xmm[1].data(), halfway, sizeof(halfway));
    run_triplet_skip_ud(s, {0xC4, 0xE3, 0x79, 0x1D, 0xC8, 0x02});
}

}  // namespace
