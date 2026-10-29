// SPDX-License-Identifier: LGPL-2.1-only
// x86-64 interpreter/JIT ↔ Oracle FMA3 tests (VEX.128.66.0F38).
//
// Same harness as test_oracle_avx_64.cpp. Fixture is OracleFma64 so
// this TU does not ODR-collide with OracleAvx64 / OracleInteger64.
//
// Finite exact lanes only (Rosetta NaN-payload drift is not a contract).
// Intel SDM Vol. 2 FMA: DEST := ±(SRC1 * SRC2) ± SRC3; flags unaffected.

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

static void put_f32(State& s, int xmm, int lane, float v) {
    std::memcpy(s.xmm[xmm].data() + lane * 4, &v, 4);
}

static void put_f64(State& s, int xmm, int lane, double v) {
    std::memcpy(s.xmm[xmm].data() + lane * 8, &v, 8);
}

static void fill_ps(State& s, int xmm, float a, float b, float c, float d) {
    put_f32(s, xmm, 0, a);
    put_f32(s, xmm, 1, b);
    put_f32(s, xmm, 2, c);
    put_f32(s, xmm, 3, d);
}

static void fill_pd(State& s, int xmm, double a, double b) {
    put_f64(s, xmm, 0, a);
    put_f64(s, xmm, 1, b);
}

class OracleFma64 : public ::testing::Test {
protected:
    void SetUp() override {
        if (!vitreo::oracle::available()) {
            GTEST_SKIP() << "oracle runner not available (requires the host x86 runner)";
        }
    }

    static State seed_state() {
        State s{};
        s.mode = vitreo::oracle::kMode64;
        s.rflags = 0x202 | vitreo::oracle::contract::kArithFlagsMask;
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

    // Exact binary32/64 values so FMA does not set MXCSR.PE.
    static State ps_state() {
        State s = seed_state();
        fill_ps(s, 0, 1.0f, 2.0f, 3.0f, 4.0f);
        fill_ps(s, 1, 0.5f, 1.5f, 2.5f, 3.5f);
        fill_ps(s, 2, 2.0f, 4.0f, 0.5f, 1.0f);
        return s;
    }

    static State pd_state() {
        State s = seed_state();
        fill_pd(s, 0, 1.0, 2.0);
        fill_pd(s, 1, 0.5, 1.5);
        fill_pd(s, 2, 2.0, 4.0);
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

// VFMADD132PS xmm0, xmm1, xmm2 — C4 E2 71 98 C2.
// DEST := DEST * SRC3 + SRC2 (Intel SDM Vol. 2 VFMADD132PS).
TEST_F(OracleFma64, Vfmadd132psXmm0Xmm1Xmm2) {
    run_triplet(ps_state(), {0xC4, 0xE2, 0x71, 0x98, 0xC2});
}

// VFMADD213PD xmm0, xmm1, xmm2 — C4 E2 F1 A8 C2.
// DEST := SRC2 * DEST + SRC3.
TEST_F(OracleFma64, Vfmadd213pdXmm0Xmm1Xmm2) {
    run_triplet(pd_state(), {0xC4, 0xE2, 0xF1, 0xA8, 0xC2});
}

// VFMADD231SS xmm0, xmm1, xmm2 — C4 E2 71 B9 C2.
// DEST[31:0] := SRC2 * SRC3 + DEST; upper dest lanes preserved.
TEST_F(OracleFma64, Vfmadd231ssXmm0Xmm1Xmm2) {
    run_triplet(ps_state(), {0xC4, 0xE2, 0x71, 0xB9, 0xC2});
}

// VFMSUB132SD xmm0, xmm1, xmm2 — C4 E2 F1 9B C2.
// DEST[63:0] := DEST * SRC3 - SRC2.
TEST_F(OracleFma64, Vfmsub132sdXmm0Xmm1Xmm2) {
    run_triplet(pd_state(), {0xC4, 0xE2, 0xF1, 0x9B, 0xC2});
}

// VFNMADD132PS xmm0, xmm1, xmm2 — C4 E2 71 9C C2.
// DEST := -(DEST * SRC3) + SRC2.
TEST_F(OracleFma64, Vfnmadd132psXmm0Xmm1Xmm2) {
    run_triplet(ps_state(), {0xC4, 0xE2, 0x71, 0x9C, 0xC2});
}

// VFNMSUB213PD xmm0, xmm1, xmm2 — C4 E2 F1 AE C2.
// DEST := -(SRC2 * DEST) - SRC3.
TEST_F(OracleFma64, Vfnmsub213pdXmm0Xmm1Xmm2) {
    run_triplet(pd_state(), {0xC4, 0xE2, 0xF1, 0xAE, 0xC2});
}

// VFMADDSUB231PS xmm0, xmm1, xmm2 — C4 E2 71 B6 C2.
// Even lanes subtract, odd lanes add (Intel SDM Vol. 2 VFMADDSUB).
TEST_F(OracleFma64, Vfmaddsub231psXmm0Xmm1Xmm2) {
    run_triplet(ps_state(), {0xC4, 0xE2, 0x71, 0xB6, 0xC2});
}

// VFMSUBADD132PD xmm0, xmm1, xmm2 — C4 E2 F1 97 C2.
// Even lanes add, odd lanes subtract.
TEST_F(OracleFma64, Vfmsubadd132pdXmm0Xmm1Xmm2) {
    run_triplet(pd_state(), {0xC4, 0xE2, 0xF1, 0x97, 0xC2});
}

// VFNMSUB132SS xmm0, xmm1, xmm2 — C4 E2 71 9F C2.
// DEST[31:0] := -(DEST * SRC3) - SRC2; upper dest lanes preserved.
TEST_F(OracleFma64, Vfnmsub132ssXmm0Xmm1Xmm2) {
    run_triplet(ps_state(), {0xC4, 0xE2, 0x71, 0x9F, 0xC2});
}

// VFNMSUB231SD xmm0, xmm1, xmm2 — C4 E2 F1 BF C2.
// DEST[63:0] := -(SRC2 * SRC3) - DEST.
TEST_F(OracleFma64, Vfnmsub231sdXmm0Xmm1Xmm2) {
    run_triplet(pd_state(), {0xC4, 0xE2, 0xF1, 0xBF, 0xC2});
}

}  // namespace
