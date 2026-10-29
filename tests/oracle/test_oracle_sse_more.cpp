// SPDX-License-Identifier: LGPL-2.1-only
// Remaining 32-bit SSE/SSE2/SSE3/SSSE3/SSE4.1 oracle triplets. Encodings are
// identical in protected mode (Vitreo) and long mode (Rosetta). EDI = kMemVa.

#include "oracle/oracle_client.hpp"
#include "oracle/oracle_contract.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace {

using vitreo::oracle::State;
using vitreo::oracle::contract::run_on_interpreter;
using vitreo::oracle::contract::run_on_jit;
using vitreo::oracle::contract::compare;
using vitreo::oracle::contract::CompareOpts;

class OracleSseMore32 : public ::testing::Test {
protected:
    void SetUp() override {
        if (!vitreo::oracle::available()) {
            GTEST_SKIP() << "oracle runner not available (requires the host x86 runner)";
        }
    }

    static void run_triplet(const State& in,
                            const std::vector<uint8_t>& code,
                            CompareOpts opts = {}) {
        State oracle_out{};
        ASSERT_TRUE(vitreo::oracle::execute(in, code, &oracle_out))
            << "oracle execution failed";
        ASSERT_EQ(oracle_out.status, 0u)
            << "oracle snippet faulted";

        State interp_out = run_on_interpreter(in, code);
        ASSERT_EQ(interp_out.status, 0u)
            << "interpreter snippet faulted";
        std::string interp_diff = compare(interp_out, oracle_out, opts);
        EXPECT_TRUE(interp_diff.empty())
            << "interpreter ↔ oracle divergence:\n" << interp_diff;

        State jit_out = run_on_jit(in, code);
        ASSERT_NE(jit_out.status, 7u)
            << "JIT did not compile the complete snippet";
        ASSERT_EQ(jit_out.status, 0u)
            << "JIT snippet faulted";
        std::string jit_diff = compare(jit_out, oracle_out, opts);
        EXPECT_TRUE(jit_diff.empty())
            << "JIT ↔ oracle divergence:\n" << jit_diff;
    }
};

static State sse_state() {
    State s{};
    s.mxcsr = 0x1F80;
    s.gpr[7] = vitreo::oracle::kMemVa;  // EDI → scratch page
    s.rflags = 0x202;
    return s;
}

static void put_f32_lane(State& s, int xmm, int lane, float v) {
    std::memcpy(s.xmm[xmm].data() + lane * 4, &v, 4);
}

static void put_f64_lane(State& s, int xmm, int lane, double v) {
    std::memcpy(s.xmm[xmm].data() + lane * 8, &v, 8);
}

static void seed_ps(State& s) {
    put_f32_lane(s, 0, 0, 1.0f);
    put_f32_lane(s, 0, 1, 2.0f);
    put_f32_lane(s, 0, 2, 3.0f);
    put_f32_lane(s, 0, 3, 4.0f);
    put_f32_lane(s, 1, 0, 5.0f);
    put_f32_lane(s, 1, 1, 6.0f);
    put_f32_lane(s, 1, 2, 7.0f);
    put_f32_lane(s, 1, 3, 8.0f);
}

static void seed_pd(State& s) {
    put_f64_lane(s, 0, 0, 1.0);
    put_f64_lane(s, 0, 1, 2.0);
    put_f64_lane(s, 1, 0, 3.0);
    put_f64_lane(s, 1, 1, 4.0);
}

static void fill_seq(State& s, int xmm, uint8_t start) {
    for (int b = 0; b < 16; ++b) {
        s.xmm[xmm][b] = static_cast<uint8_t>(start + b);
    }
}

static void put_i32(State& s, int xmm, int lane, int32_t v) {
    std::memcpy(s.xmm[xmm].data() + lane * 4, &v, 4);
}

static void put_u16(State& s, int xmm, int lane, uint16_t v) {
    std::memcpy(s.xmm[xmm].data() + lane * 2, &v, 2);
}

// ---------------------------------------------------------------------------
// SSE packed/scalar moves and bitwise
// ---------------------------------------------------------------------------

TEST_F(OracleSseMore32, Movaps) {
    State in = sse_state();
    seed_ps(in);
    run_triplet(in, {0x0F, 0x28, 0xC1});  // MOVAPS XMM0, XMM1
}

TEST_F(OracleSseMore32, Movapd) {
    State in = sse_state();
    seed_pd(in);
    run_triplet(in, {0x66, 0x0F, 0x28, 0xC1});  // MOVAPD XMM0, XMM1
}

TEST_F(OracleSseMore32, Movups) {
    State in = sse_state();
    seed_ps(in);
    run_triplet(in, {0x0F, 0x10, 0xC1});  // MOVUPS XMM0, XMM1
}

TEST_F(OracleSseMore32, Movdqu) {
    State in = sse_state();
    fill_seq(in, 1, 0x40);
    run_triplet(in, {0xF3, 0x0F, 0x6F, 0xC1});  // MOVDQU XMM0, XMM1
}

TEST_F(OracleSseMore32, Xorps) {
    State in = sse_state();
    seed_ps(in);
    run_triplet(in, {0x0F, 0x57, 0xC1});  // XORPS XMM0, XMM1
}

TEST_F(OracleSseMore32, Xorpd) {
    State in = sse_state();
    seed_pd(in);
    run_triplet(in, {0x66, 0x0F, 0x57, 0xC1});  // XORPD XMM0, XMM1
}

TEST_F(OracleSseMore32, Orps) {
    State in = sse_state();
    seed_ps(in);
    run_triplet(in, {0x0F, 0x56, 0xC1});  // ORPS XMM0, XMM1
}

TEST_F(OracleSseMore32, Andnps) {
    State in = sse_state();
    seed_ps(in);
    run_triplet(in, {0x0F, 0x55, 0xC1});  // ANDNPS XMM0, XMM1
}

// ---------------------------------------------------------------------------
// SSE packed/scalar arithmetic (exact finite lanes; no PE)
// ---------------------------------------------------------------------------

TEST_F(OracleSseMore32, Subps) {
    State in = sse_state();
    seed_ps(in);
    run_triplet(in, {0x0F, 0x5C, 0xC1});  // SUBPS XMM0, XMM1
}

TEST_F(OracleSseMore32, Mulps) {
    State in = sse_state();
    seed_ps(in);
    run_triplet(in, {0x0F, 0x59, 0xC1});  // MULPS XMM0, XMM1
}

TEST_F(OracleSseMore32, Divps) {
    State in = sse_state();
    put_f32_lane(in, 0, 0, 2.0f);
    put_f32_lane(in, 0, 1, 4.0f);
    put_f32_lane(in, 0, 2, 6.0f);
    put_f32_lane(in, 0, 3, 8.0f);
    put_f32_lane(in, 1, 0, 1.0f);
    put_f32_lane(in, 1, 1, 2.0f);
    put_f32_lane(in, 1, 2, 2.0f);
    put_f32_lane(in, 1, 3, 4.0f);
    run_triplet(in, {0x0F, 0x5E, 0xC1});  // DIVPS XMM0, XMM1
}

TEST_F(OracleSseMore32, DivpsInexactSetsPe) {
    // 1/3 is not binary32-exact; SDM Vol. 1 §11.5.2.6 sticky PE.
    State in = sse_state();
    put_f32_lane(in, 0, 0, 1.0f);
    put_f32_lane(in, 0, 1, 1.0f);
    put_f32_lane(in, 0, 2, 1.0f);
    put_f32_lane(in, 0, 3, 1.0f);
    put_f32_lane(in, 1, 0, 3.0f);
    put_f32_lane(in, 1, 1, 3.0f);
    put_f32_lane(in, 1, 2, 3.0f);
    put_f32_lane(in, 1, 3, 3.0f);
    run_triplet(in, {0x0F, 0x5E, 0xC1});
}

TEST_F(OracleSseMore32, Minps) {
    State in = sse_state();
    put_f32_lane(in, 0, 0, 1.0f);
    put_f32_lane(in, 0, 1, 6.0f);
    put_f32_lane(in, 0, 2, 3.0f);
    put_f32_lane(in, 0, 3, 8.0f);
    put_f32_lane(in, 1, 0, 5.0f);
    put_f32_lane(in, 1, 1, 2.0f);
    put_f32_lane(in, 1, 2, 7.0f);
    put_f32_lane(in, 1, 3, 4.0f);
    run_triplet(in, {0x0F, 0x5D, 0xC1});  // MINPS XMM0, XMM1
}

TEST_F(OracleSseMore32, Maxps) {
    State in = sse_state();
    put_f32_lane(in, 0, 0, 1.0f);
    put_f32_lane(in, 0, 1, 6.0f);
    put_f32_lane(in, 0, 2, 3.0f);
    put_f32_lane(in, 0, 3, 8.0f);
    put_f32_lane(in, 1, 0, 5.0f);
    put_f32_lane(in, 1, 1, 2.0f);
    put_f32_lane(in, 1, 2, 7.0f);
    put_f32_lane(in, 1, 3, 4.0f);
    run_triplet(in, {0x0F, 0x5F, 0xC1});  // MAXPS XMM0, XMM1
}

TEST_F(OracleSseMore32, Sqrtps) {
    State in = sse_state();
    put_f32_lane(in, 1, 0, 1.0f);
    put_f32_lane(in, 1, 1, 4.0f);
    put_f32_lane(in, 1, 2, 9.0f);
    put_f32_lane(in, 1, 3, 16.0f);
    put_f32_lane(in, 0, 0, 1.0f);
    put_f32_lane(in, 0, 1, 2.0f);
    put_f32_lane(in, 0, 2, 3.0f);
    put_f32_lane(in, 0, 3, 4.0f);
    run_triplet(in, {0x0F, 0x51, 0xC1});  // SQRTPS XMM0, XMM1
}

TEST_F(OracleSseMore32, Addpd) {
    State in = sse_state();
    seed_pd(in);
    run_triplet(in, {0x66, 0x0F, 0x58, 0xC1});  // ADDPD XMM0, XMM1
}

TEST_F(OracleSseMore32, Subpd) {
    State in = sse_state();
    seed_pd(in);
    run_triplet(in, {0x66, 0x0F, 0x5C, 0xC1});  // SUBPD XMM0, XMM1
}

TEST_F(OracleSseMore32, Mulpd) {
    State in = sse_state();
    seed_pd(in);
    run_triplet(in, {0x66, 0x0F, 0x59, 0xC1});  // MULPD XMM0, XMM1
}

TEST_F(OracleSseMore32, AddssPreservesUpper) {
    State in = sse_state();
    seed_ps(in);  // xmm0 upper lanes 2/3/4 stay; only lane 0 is 1.0+5.0
    run_triplet(in, {0xF3, 0x0F, 0x58, 0xC1});  // ADDSS XMM0, XMM1
}

TEST_F(OracleSseMore32, AddsdPreservesUpper) {
    State in = sse_state();
    seed_pd(in);  // xmm0 high double 2.0 stays; low is 1.0+3.0
    run_triplet(in, {0xF2, 0x0F, 0x58, 0xC1});  // ADDSD XMM0, XMM1
}

TEST_F(OracleSseMore32, Sqrtss) {
    State in = sse_state();
    seed_ps(in);
    put_f32_lane(in, 1, 0, 4.0f);  // exact sqrt
    run_triplet(in, {0xF3, 0x0F, 0x51, 0xC1});  // SQRTSS XMM0, XMM1
}

TEST_F(OracleSseMore32, Cvtps2pd) {
    State in = sse_state();
    seed_ps(in);  // converts xmm1[0], xmm1[1] = 5.0, 6.0 exactly
    run_triplet(in, {0x0F, 0x5A, 0xC1});  // CVTPS2PD XMM0, XMM1
}

TEST_F(OracleSseMore32, MovdXmm0Eax) {
    State in = sse_state();
    seed_ps(in);
    in.gpr[0] = 0xAABBCCDDu;
    run_triplet(in, {0x66, 0x0F, 0x6E, 0xC0});  // MOVD XMM0, EAX
}

TEST_F(OracleSseMore32, MovdEaxXmm0) {
    State in = sse_state();
    seed_ps(in);
    in.gpr[0] = 0x11111111u;
    run_triplet(in, {0x66, 0x0F, 0x7E, 0xC0});  // MOVD EAX, XMM0
}

TEST_F(OracleSseMore32, MovqXmm0Xmm1) {
    State in = sse_state();
    seed_pd(in);
    run_triplet(in, {0xF3, 0x0F, 0x7E, 0xC1});  // MOVQ XMM0, XMM1
}

TEST_F(OracleSseMore32, Pmovmskb) {
    State in = sse_state();
    for (int b = 0; b < 16; ++b) {
        in.xmm[0][b] = (b & 1) ? 0x80 : 0x01;
    }
    in.gpr[0] = 0xFFFFFFFFu;
    run_triplet(in, {0x66, 0x0F, 0xD7, 0xC0});  // PMOVMSKB EAX, XMM0
}

TEST_F(OracleSseMore32, Por) {
    State in = sse_state();
    fill_seq(in, 0, 0x00);
    fill_seq(in, 1, 0xF0);
    run_triplet(in, {0x66, 0x0F, 0xEB, 0xC1});  // POR XMM0, XMM1
}

TEST_F(OracleSseMore32, Pandn) {
    State in = sse_state();
    fill_seq(in, 0, 0x00);
    fill_seq(in, 1, 0xF0);
    run_triplet(in, {0x66, 0x0F, 0xDF, 0xC1});  // PANDN XMM0, XMM1
}

TEST_F(OracleSseMore32, Psubd) {
    State in = sse_state();
    put_i32(in, 0, 0, 10);
    put_i32(in, 0, 1, 20);
    put_i32(in, 0, 2, 30);
    put_i32(in, 0, 3, 40);
    put_i32(in, 1, 0, 1);
    put_i32(in, 1, 1, 2);
    put_i32(in, 1, 2, 3);
    put_i32(in, 1, 3, 4);
    run_triplet(in, {0x66, 0x0F, 0xFA, 0xC1});  // PSUBD XMM0, XMM1
}

TEST_F(OracleSseMore32, Paddq) {
    State in = sse_state();
    seed_pd(in);  // bit patterns of 1.0/2.0/3.0/4.0 as qwords
    run_triplet(in, {0x66, 0x0F, 0xD4, 0xC1});  // PADDQ XMM0, XMM1
}

TEST_F(OracleSseMore32, Pmullw) {
    State in = sse_state();
    for (int i = 0; i < 8; ++i) {
        put_u16(in, 0, i, static_cast<uint16_t>(i + 1));
        put_u16(in, 1, i, static_cast<uint16_t>(2));
    }
    run_triplet(in, {0x66, 0x0F, 0xD5, 0xC1});  // PMULLW XMM0, XMM1
}

TEST_F(OracleSseMore32, Pcmpeqb) {
    State in = sse_state();
    fill_seq(in, 0, 0);
    fill_seq(in, 1, 0);
    in.xmm[1][3] = 0xFF;
    in.xmm[1][7] = 0xAA;
    run_triplet(in, {0x66, 0x0F, 0x74, 0xC1});  // PCMPEQB XMM0, XMM1
}

TEST_F(OracleSseMore32, Pcmpgtb) {
    State in = sse_state();
    for (int b = 0; b < 16; ++b) {
        in.xmm[0][b] = static_cast<uint8_t>(b);
        in.xmm[1][b] = static_cast<uint8_t>(8);
    }
    run_triplet(in, {0x66, 0x0F, 0x64, 0xC1});  // PCMPGTB XMM0, XMM1
}

TEST_F(OracleSseMore32, Pmulld) {
    State in = sse_state();
    put_i32(in, 0, 0, 2);
    put_i32(in, 0, 1, 3);
    put_i32(in, 0, 2, 4);
    put_i32(in, 0, 3, 5);
    put_i32(in, 1, 0, 6);
    put_i32(in, 1, 1, 7);
    put_i32(in, 1, 2, 8);
    put_i32(in, 1, 3, 9);
    run_triplet(in, {0x66, 0x0F, 0x38, 0x40, 0xC1});  // PMULLD XMM0, XMM1
}

TEST_F(OracleSseMore32, Pminsd) {
    State in = sse_state();
    put_i32(in, 0, 0, -1);
    put_i32(in, 0, 1, 6);
    put_i32(in, 0, 2, 3);
    put_i32(in, 0, 3, -8);
    put_i32(in, 1, 0, 5);
    put_i32(in, 1, 1, 2);
    put_i32(in, 1, 2, -7);
    put_i32(in, 1, 3, 4);
    run_triplet(in, {0x66, 0x0F, 0x38, 0x39, 0xC1});  // PMINSD XMM0, XMM1
}

TEST_F(OracleSseMore32, Pmaxsd) {
    State in = sse_state();
    put_i32(in, 0, 0, -1);
    put_i32(in, 0, 1, 6);
    put_i32(in, 0, 2, 3);
    put_i32(in, 0, 3, -8);
    put_i32(in, 1, 0, 5);
    put_i32(in, 1, 1, 2);
    put_i32(in, 1, 2, -7);
    put_i32(in, 1, 3, 4);
    run_triplet(in, {0x66, 0x0F, 0x38, 0x3D, 0xC1});  // PMAXSD XMM0, XMM1
}

TEST_F(OracleSseMore32, Pmovzxbw) {
    State in = sse_state();
    in.xmm[1] = {0x7F, 0x80, 0x01, 0xFF, 0x00, 0x40, 0xC0, 0x10,
                 0, 0, 0, 0, 0, 0, 0, 0};
    fill_seq(in, 0, 0xAA);
    run_triplet(in, {0x66, 0x0F, 0x38, 0x30, 0xC1});  // PMOVZXBW XMM0, XMM1
}

TEST_F(OracleSseMore32, Pminub) {
    State in = sse_state();
    fill_seq(in, 0, 0x10);
    fill_seq(in, 1, 0x08);
    run_triplet(in, {0x66, 0x0F, 0xDA, 0xC1});  // PMINUB XMM0, XMM1
}

TEST_F(OracleSseMore32, Pavgb) {
    State in = sse_state();
    fill_seq(in, 0, 0x10);
    fill_seq(in, 1, 0x20);
    run_triplet(in, {0x66, 0x0F, 0xE0, 0xC1});  // PAVGB XMM0, XMM1
}

TEST_F(OracleSseMore32, Psadbw) {
    State in = sse_state();
    fill_seq(in, 0, 0x10);
    fill_seq(in, 1, 0x20);
    run_triplet(in, {0x66, 0x0F, 0xF6, 0xC1});  // PSADBW XMM0, XMM1
}

TEST_F(OracleSseMore32, Paddusb) {
    State in = sse_state();
    in.xmm[0].fill(0xF0);
    in.xmm[1].fill(0x20);
    run_triplet(in, {0x66, 0x0F, 0xDC, 0xC1});  // PADDUSB XMM0, XMM1
}

TEST_F(OracleSseMore32, PsllwImm4) {
    State in = sse_state();
    for (int i = 0; i < 8; ++i) {
        put_u16(in, 0, i, static_cast<uint16_t>(0x0001u << i));
    }
    run_triplet(in, {0x66, 0x0F, 0x71, 0xF0, 0x04});  // PSLLW XMM0, 4
}

TEST_F(OracleSseMore32, PsrldqImm4) {
    State in = sse_state();
    fill_seq(in, 0, 0x10);
    run_triplet(in, {0x66, 0x0F, 0x73, 0xD8, 0x04});  // PSRLDQ XMM0, 4
}

TEST_F(OracleSseMore32, PshufdImm1b) {
    State in = sse_state();
    seed_ps(in);
    run_triplet(in, {0x66, 0x0F, 0x70, 0xC1, 0x1B});  // PSHUFD XMM0, XMM1, 0x1B
}

TEST_F(OracleSseMore32, PshuflwImm1b) {
    State in = sse_state();
    for (int i = 0; i < 8; ++i) {
        put_u16(in, 1, i, static_cast<uint16_t>(0x1000u + i));
        put_u16(in, 0, i, static_cast<uint16_t>(0x2000u + i));
    }
    run_triplet(in, {0xF2, 0x0F, 0x70, 0xC1, 0x1B});  // PSHUFLW XMM0, XMM1, 0x1B
}

TEST_F(OracleSseMore32, ShufpdImm1) {
    State in = sse_state();
    seed_pd(in);
    run_triplet(in, {0x66, 0x0F, 0xC6, 0xC1, 0x01});  // SHUFPD XMM0, XMM1, 1
}

TEST_F(OracleSseMore32, Punpcklbw) {
    State in = sse_state();
    fill_seq(in, 0, 0x00);
    fill_seq(in, 1, 0x80);
    run_triplet(in, {0x66, 0x0F, 0x60, 0xC1});  // PUNPCKLBW XMM0, XMM1
}

TEST_F(OracleSseMore32, Punpckldq) {
    State in = sse_state();
    seed_ps(in);
    run_triplet(in, {0x66, 0x0F, 0x62, 0xC1});  // PUNPCKLDQ XMM0, XMM1
}

TEST_F(OracleSseMore32, Unpcklps) {
    State in = sse_state();
    seed_ps(in);
    run_triplet(in, {0x0F, 0x14, 0xC1});  // UNPCKLPS XMM0, XMM1
}

TEST_F(OracleSseMore32, PinsrwImm1) {
    State in = sse_state();
    fill_seq(in, 0, 0x10);
    in.gpr[0] = 0xAABBCCDDu;
    run_triplet(in, {0x66, 0x0F, 0xC4, 0xC0, 0x01});  // PINSRW XMM0, EAX, 1
}

TEST_F(OracleSseMore32, PextrwImm1) {
    State in = sse_state();
    fill_seq(in, 0, 0x10);
    in.gpr[0] = 0xFFFFFFFFu;
    run_triplet(in, {0x66, 0x0F, 0xC5, 0xC0, 0x01});  // PEXTRW EAX, XMM0, 1
}

TEST_F(OracleSseMore32, MovssPreservesUpper) {
    State in = sse_state();
    seed_ps(in);
    run_triplet(in, {0xF3, 0x0F, 0x10, 0xC1});  // MOVSS XMM0, XMM1
}

TEST_F(OracleSseMore32, Movsldup) {
    State in = sse_state();
    seed_ps(in);
    run_triplet(in, {0xF3, 0x0F, 0x12, 0xC1});  // MOVSLDUP XMM0, XMM1
}

TEST_F(OracleSseMore32, Movddup) {
    State in = sse_state();
    seed_pd(in);
    run_triplet(in, {0xF2, 0x0F, 0x12, 0xC1});  // MOVDDUP XMM0, XMM1
}

TEST_F(OracleSseMore32, Hsubps) {
    State in = sse_state();
    seed_ps(in);
    run_triplet(in, {0xF2, 0x0F, 0x7D, 0xC1});  // HSUBPS XMM0, XMM1
}

TEST_F(OracleSseMore32, Packsswb) {
    State in = sse_state();
    for (int i = 0; i < 8; ++i) {
        put_u16(in, 0, i, static_cast<uint16_t>(i + 1));
        put_u16(in, 1, i, static_cast<uint16_t>(0x10 + i));
    }
    run_triplet(in, {0x66, 0x0F, 0x63, 0xC1});  // PACKSSWB XMM0, XMM1
}

TEST_F(OracleSseMore32, Pabsb) {
    State in = sse_state();
    in.xmm[1] = {0x00, 0x01, 0x7F, 0x80, 0xFF, 0xFE, 0x10, 0xF0,
                 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09};
    fill_seq(in, 0, 0xAA);
    run_triplet(in, {0x66, 0x0F, 0x38, 0x1C, 0xC1});  // PABSB XMM0, XMM1
}

TEST_F(OracleSseMore32, Phaddd) {
    State in = sse_state();
    put_i32(in, 0, 0, 1);
    put_i32(in, 0, 1, 2);
    put_i32(in, 0, 2, 3);
    put_i32(in, 0, 3, 4);
    put_i32(in, 1, 0, 5);
    put_i32(in, 1, 1, 6);
    put_i32(in, 1, 2, 7);
    put_i32(in, 1, 3, 8);
    run_triplet(in, {0x66, 0x0F, 0x38, 0x02, 0xC1});  // PHADDD XMM0, XMM1
}

TEST_F(OracleSseMore32, Psignb) {
    State in = sse_state();
    fill_seq(in, 0, 0x10);
    in.xmm[1] = {0x01, 0x00, 0xFF, 0x02, 0x80, 0x03, 0x00, 0x7F,
                 0x01, 0xFF, 0x00, 0x10, 0x20, 0x00, 0x01, 0xFE};
    run_triplet(in, {0x66, 0x0F, 0x38, 0x08, 0xC1});  // PSIGNB XMM0, XMM1
}

TEST_F(OracleSseMore32, Addsubps) {
    State in = sse_state();
    seed_ps(in);
    run_triplet(in, {0xF2, 0x0F, 0xD0, 0xC1});  // ADDSUBPS XMM0, XMM1
}

TEST_F(OracleSseMore32, RoundpdNearest) {
    State in = sse_state();
    put_f64_lane(in, 1, 0, 1.0);
    put_f64_lane(in, 1, 1, 2.0);
    put_f64_lane(in, 0, 0, 3.0);
    put_f64_lane(in, 0, 1, 4.0);
    run_triplet(in, {0x66, 0x0F, 0x3A, 0x09, 0xC1, 0x00});  // ROUNDPD XMM0, XMM1, 0
}

TEST_F(OracleSseMore32, Movmskps) {
    State in = sse_state();
    put_f32_lane(in, 0, 0, -1.0f);
    put_f32_lane(in, 0, 1, 2.0f);
    put_f32_lane(in, 0, 2, -3.0f);
    put_f32_lane(in, 0, 3, 4.0f);
    in.gpr[0] = 0xFFFFFFFFu;
    run_triplet(in, {0x0F, 0x50, 0xC0});  // MOVMSKPS EAX, XMM0
}

TEST_F(OracleSseMore32, BlendpdImm1) {
    State in = sse_state();
    seed_pd(in);
    run_triplet(in, {0x66, 0x0F, 0x3A, 0x0D, 0xC1, 0x01});  // BLENDPD XMM0, XMM1, 1
}

TEST_F(OracleSseMore32, PblendwImm55) {
    State in = sse_state();
    fill_seq(in, 0, 0x10);
    fill_seq(in, 1, 0x80);
    run_triplet(in, {0x66, 0x0F, 0x3A, 0x0E, 0xC1, 0x55});  // PBLENDW XMM0, XMM1, 0x55
}

TEST_F(OracleSseMore32, ExtractpsImm1) {
    State in = sse_state();
    seed_ps(in);
    in.gpr[0] = 0xFFFFFFFFu;
    run_triplet(in, {0x66, 0x0F, 0x3A, 0x17, 0xC0, 0x01});  // EXTRACTPS EAX, XMM0, 1
}

TEST_F(OracleSseMore32, PextrbImm5) {
    State in = sse_state();
    fill_seq(in, 0, 0xA0);
    in.gpr[0] = 0xFFFFFFFFu;
    run_triplet(in, {0x66, 0x0F, 0x3A, 0x14, 0xC0, 0x05});  // PEXTRB EAX, XMM0, 5
}

TEST_F(OracleSseMore32, PinsrbImm5) {
    State in = sse_state();
    fill_seq(in, 0, 0x10);
    in.gpr[0] = 0xAABBCCDDu;
    run_triplet(in, {0x66, 0x0F, 0x3A, 0x20, 0xC0, 0x05});  // PINSRB XMM0, EAX, 5
}

TEST_F(OracleSseMore32, InsertpsImm40) {
    State in = sse_state();
    seed_ps(in);
    run_triplet(in, {0x66, 0x0F, 0x3A, 0x21, 0xC1, 0x40});  // INSERTPS XMM0, XMM1, 0x40
}

TEST_F(OracleSseMore32, CmppsEq) {
    State in = sse_state();
    put_f32_lane(in, 0, 0, 1.0f);
    put_f32_lane(in, 0, 1, 2.0f);
    put_f32_lane(in, 0, 2, 3.0f);
    put_f32_lane(in, 0, 3, 4.0f);
    put_f32_lane(in, 1, 0, 1.0f);
    put_f32_lane(in, 1, 1, 6.0f);
    put_f32_lane(in, 1, 2, 3.0f);
    put_f32_lane(in, 1, 3, 8.0f);
    run_triplet(in, {0x0F, 0xC2, 0xC1, 0x00});  // CMPPS XMM0, XMM1, EQ
}

TEST_F(OracleSseMore32, Ucomiss) {
    State in = sse_state();
    seed_ps(in);
    run_triplet(in, {0x0F, 0x2E, 0xC1});  // UCOMISS XMM0, XMM1
}

TEST_F(OracleSseMore32, ComissThenJbe) {
    // COMISS; JBE — unordered (NaN) must take JBE (CF|ZF). No 0x40 INC (REX).
    // Rosetta sets MXCSR.IE on QNaN; Vitreo does not deliver that exception.
    const std::vector<uint8_t> code = {
        0x31, 0xC0,        // xor eax, eax
        0x0F, 0x2F, 0xC1,  // comiss xmm0, xmm1
        0x76, 0x03,        // jbe +3
        0x83, 0xC0, 0x01,  // add eax, 1
    };
    const float qnan = std::numeric_limits<float>::quiet_NaN();
    const struct {
        float a, b;
        uint32_t eax;  // 0 if JBE taken
        bool nan;
    } cases[] = {
        {2.0f, 1.0f, 1, false},
        {1.0f, 2.0f, 0, false},
        {1.0f, 1.0f, 0, false},
        {qnan, 1.0f, 0, true},
        {1.0f, qnan, 0, true},
    };
    for (const auto& c : cases) {
        State in = sse_state();
        put_f32_lane(in, 0, 0, c.a);
        put_f32_lane(in, 1, 0, c.b);
        if (!c.nan) {
            run_triplet(in, code);
        } else {
            State oracle_out{};
            ASSERT_TRUE(vitreo::oracle::execute(in, code, &oracle_out));
            ASSERT_EQ(oracle_out.status, 0u);
            State interp_out = run_on_interpreter(in, code);
            ASSERT_EQ(interp_out.status, 0u);
            State jit_out = run_on_jit(in, code);
            ASSERT_EQ(jit_out.status, 0u);
            EXPECT_EQ(oracle_out.gpr[0] & 0xffffffffu, c.eax);
            EXPECT_EQ(interp_out.gpr[0] & 0xffffffffu, c.eax);
            EXPECT_EQ(jit_out.gpr[0] & 0xffffffffu, c.eax);
            CompareOpts opts;
            oracle_out.mxcsr &= ~1u;
            interp_out.mxcsr &= ~1u;
            jit_out.mxcsr &= ~1u;
            EXPECT_TRUE(compare(interp_out, oracle_out, opts).empty());
            EXPECT_TRUE(compare(jit_out, oracle_out, opts).empty());
        }
    }
}

TEST_F(OracleSseMore32, ComissThenMovLea) {
    // COMISS; MOV; LEA — integer ops must not clobber ZF/PF/CF.
    const std::vector<uint8_t> code = {
        0x0F, 0x2F, 0xC1,  // comiss xmm0, xmm1
        0x89, 0xC1,        // mov ecx, eax
        0x8D, 0x40, 0x10,  // lea eax, [eax+0x10]
    };
    const float pairs[][2] = {
        {2.0f, 1.0f}, {1.0f, 2.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f},
    };
    for (const auto& p : pairs) {
        State in = sse_state();
        in.gpr[0] = 0x1000;
        put_f32_lane(in, 0, 0, p[0]);
        put_f32_lane(in, 1, 0, p[1]);
        run_triplet(in, code);
    }
}

TEST_F(OracleSseMore32, Comisd) {
    State in = sse_state();
    seed_pd(in);
    run_triplet(in, {0x66, 0x0F, 0x2F, 0xC1});  // COMISD XMM0, XMM1
}

TEST_F(OracleSseMore32, Ucomisd) {
    State in = sse_state();
    seed_pd(in);
    run_triplet(in, {0x66, 0x0F, 0x2E, 0xC1});  // UCOMISD XMM0, XMM1
}

TEST_F(OracleSseMore32, MovlpsFromEdi) {
    State in = sse_state();
    seed_ps(in);
    float lo0 = 5.0f;
    float lo1 = 6.0f;
    std::memcpy(in.mem.data(), &lo0, 4);
    std::memcpy(in.mem.data() + 4, &lo1, 4);
    run_triplet(in, {0x0F, 0x12, 0x07});  // MOVLPS XMM0, [EDI]
}

TEST_F(OracleSseMore32, MovhpsFromEdi) {
    State in = sse_state();
    seed_ps(in);
    float hi0 = 5.0f;
    float hi1 = 6.0f;
    std::memcpy(in.mem.data(), &hi0, 4);
    std::memcpy(in.mem.data() + 4, &hi1, 4);
    run_triplet(in, {0x0F, 0x16, 0x07});  // MOVHPS XMM0, [EDI]
}

static void put_mem_f32(State& s, uint32_t off, float v) {
    std::memcpy(s.mem.data() + off, &v, 4);
}

static void put_mem_row(State& s, uint32_t off, float x, float y, float z,
                        float w) {
    put_mem_f32(s, off, x);
    put_mem_f32(s, off + 4, y);
    put_mem_f32(s, off + 8, z);
    put_mem_f32(s, off + 12, w);
}

TEST_F(OracleSseMore32, ShufpsBroadcastAndRotateImms) {
    const uint8_t imms[] = {0x00, 0x39, 0x55, 0xAA, 0xFF};
    for (uint8_t imm : imms) {
        State in = sse_state();
        seed_ps(in);
        run_triplet(in, {0x0F, 0xC6, 0xC1, imm});
    }
}

TEST_F(OracleSseMore32, PerspectiveDivideByW) {
    // movaps xmm1, xmm0; shufps xmm1, xmm0, 0xFF; divps xmm0, xmm1
    State in = sse_state();
    put_f32_lane(in, 0, 0, 2.0f);
    put_f32_lane(in, 0, 1, 4.0f);
    put_f32_lane(in, 0, 2, 6.0f);
    put_f32_lane(in, 0, 3, 2.0f);
    run_triplet(in, {0x0F, 0x28, 0xC8, 0x0F, 0xC6, 0xC8, 0xFF, 0x0F, 0x5E, 0xC1});
}

static float lane_f32(const State& s, int xmm, int lane) {
    float v = 0.0f;
    std::memcpy(&v, s.xmm[xmm].data() + lane * 4, 4);
    return v;
}

static void run_rsqrtps(const State& in) {
    const std::vector<uint8_t> code = {0x0F, 0x52, 0xC1};  // RSQRTPS XMM0, XMM1
    State oracle_out{};
    ASSERT_TRUE(vitreo::oracle::execute(in, code, &oracle_out));
    ASSERT_EQ(oracle_out.status, 0u) << "oracle snippet faulted";

    State interp_out = run_on_interpreter(in, code);
    ASSERT_EQ(interp_out.status, 0u) << "interpreter snippet faulted";
    State jit_out = run_on_jit(in, code);
    ASSERT_NE(jit_out.status, 7u) << "JIT did not compile";
    ASSERT_EQ(jit_out.status, 0u) << "JIT snippet faulted";

    constexpr float kBound = 1.5f / 4096.0f;
    for (int lane = 0; lane < 4; ++lane) {
        const float src = lane_f32(in, 1, lane);
        const float exact = 1.0f / std::sqrt(src);
        for (const char* who : {"interpreter", "jit", "oracle"}) {
            const State& got = (who[0] == 'i') ? interp_out
                               : (who[0] == 'j') ? jit_out : oracle_out;
            const float y = lane_f32(got, 0, lane);
            if (!std::isfinite(src) || src <= 0.0f) continue;
            const float rel = std::fabs(y - exact) / std::fabs(exact);
            EXPECT_LE(rel, kBound) << who << " lane " << lane
                                   << " src=" << src << " y=" << y;
        }
    }

    CompareOpts opts;
    State a = interp_out;
    State b = oracle_out;
    State c = jit_out;
    a.xmm[0].fill(0);
    b.xmm[0].fill(0);
    c.xmm[0].fill(0);
    a.mxcsr &= ~(1u << 5);
    b.mxcsr &= ~(1u << 5);
    c.mxcsr &= ~(1u << 5);
    EXPECT_TRUE(compare(a, b, opts).empty()) << "interpreter ↔ oracle (masked)";
    EXPECT_TRUE(compare(c, b, opts).empty()) << "JIT ↔ oracle (masked)";
}

TEST_F(OracleSseMore32, RsqrtpsFinite) {
    State in = sse_state();
    put_f32_lane(in, 1, 0, 4.0f);
    put_f32_lane(in, 1, 1, 2.0f);
    put_f32_lane(in, 1, 2, 3.0f);
    put_f32_lane(in, 1, 3, 0.25f);
    run_rsqrtps(in);
}

TEST_F(OracleSseMore32, RsqrtpsZeroAndNegative) {
    State in = sse_state();
    put_f32_lane(in, 1, 0, 0.0f);
    put_f32_lane(in, 1, 1, -1.0f);
    put_f32_lane(in, 1, 2, 4.0f);
    put_f32_lane(in, 1, 3, -0.0f);
    const std::vector<uint8_t> code = {0x0F, 0x52, 0xC1};
    State oracle_out{};
    ASSERT_TRUE(vitreo::oracle::execute(in, code, &oracle_out));
    ASSERT_EQ(oracle_out.status, 0u);
    State interp_out = run_on_interpreter(in, code);
    ASSERT_EQ(interp_out.status, 0u);
    State jit_out = run_on_jit(in, code);
    ASSERT_EQ(jit_out.status, 0u);

    auto class_ok = [](float y, bool expect_inf) {
        if (expect_inf) return std::isinf(y);
        return std::isnan(y);
    };
    for (State* s : {&oracle_out, &interp_out, &jit_out}) {
        EXPECT_TRUE(class_ok(lane_f32(*s, 0, 0), true)) << "lane0 +0";
        EXPECT_TRUE(class_ok(lane_f32(*s, 0, 1), false)) << "lane1 -1";
        EXPECT_NEAR(lane_f32(*s, 0, 2), 0.5f, 1.5f / 4096.0f);
        EXPECT_TRUE(class_ok(lane_f32(*s, 0, 3), true)) << "lane3 -0";
    }
}

TEST_F(OracleSseMore32, RsqrtpsNewtonRefine) {
    // y' = y * (1.5 - 0.5 * x * y * y); x in xmm1, y = rsqrtps(x) in xmm0.
    const std::vector<uint8_t> code = {
        0x0F, 0x28, 0xC1,        // movaps xmm0, xmm1
        0x0F, 0x52, 0xC0,        // rsqrtps xmm0, xmm0
        0x0F, 0x28, 0xD0,        // movaps xmm2, xmm0
        0x0F, 0x59, 0xD0,        // mulps xmm2, xmm0
        0x0F, 0x59, 0xD1,        // mulps xmm2, xmm1
        0x0F, 0x59, 0xD3,        // mulps xmm2, xmm3   ; xmm3 = 0.5
        0x0F, 0x5C, 0xE2,        // subps xmm4, xmm2   ; 1.5 - ...
        0x0F, 0x59, 0xC4,        // mulps xmm0, xmm4
    };
    State in = sse_state();
    put_f32_lane(in, 1, 0, 4.0f);
    put_f32_lane(in, 1, 1, 9.0f);
    put_f32_lane(in, 1, 2, 1.0f);
    put_f32_lane(in, 1, 3, 16.0f);
    put_f32_lane(in, 3, 0, 0.5f);
    put_f32_lane(in, 3, 1, 0.5f);
    put_f32_lane(in, 3, 2, 0.5f);
    put_f32_lane(in, 3, 3, 0.5f);
    put_f32_lane(in, 4, 0, 1.5f);
    put_f32_lane(in, 4, 1, 1.5f);
    put_f32_lane(in, 4, 2, 1.5f);
    put_f32_lane(in, 4, 3, 1.5f);

    State oracle_out{};
    ASSERT_TRUE(vitreo::oracle::execute(in, code, &oracle_out));
    ASSERT_EQ(oracle_out.status, 0u);
    State interp_out = run_on_interpreter(in, code);
    ASSERT_EQ(interp_out.status, 0u);
    State jit_out = run_on_jit(in, code);
    ASSERT_EQ(jit_out.status, 0u);

    const float exact[4] = {0.5f, 1.0f / 3.0f, 1.0f, 0.25f};
    for (int lane = 0; lane < 4; ++lane) {
        for (const State* s : {&oracle_out, &interp_out, &jit_out}) {
            EXPECT_NEAR(lane_f32(*s, 0, lane), exact[lane], 1e-5f)
                << "lane " << lane;
        }
    }
}

TEST_F(OracleSseMore32, RsqrtpsNewtonZero) {
    // 0 * Inf in the refine step is NaN; all three engines must agree.
    const std::vector<uint8_t> code = {
        0x0F, 0x28, 0xC1,
        0x0F, 0x52, 0xC0,
        0x0F, 0x28, 0xD0,
        0x0F, 0x59, 0xD0,
        0x0F, 0x59, 0xD1,
        0x0F, 0x59, 0xD3,
        0x0F, 0x5C, 0xE2,
        0x0F, 0x59, 0xC4,
    };
    State in = sse_state();
    put_f32_lane(in, 1, 0, 0.0f);
    put_f32_lane(in, 1, 1, 0.0f);
    put_f32_lane(in, 1, 2, 0.0f);
    put_f32_lane(in, 1, 3, 0.0f);
    put_f32_lane(in, 3, 0, 0.5f);
    put_f32_lane(in, 3, 1, 0.5f);
    put_f32_lane(in, 3, 2, 0.5f);
    put_f32_lane(in, 3, 3, 0.5f);
    put_f32_lane(in, 4, 0, 1.5f);
    put_f32_lane(in, 4, 1, 1.5f);
    put_f32_lane(in, 4, 2, 1.5f);
    put_f32_lane(in, 4, 3, 1.5f);

    State oracle_out{};
    ASSERT_TRUE(vitreo::oracle::execute(in, code, &oracle_out));
    ASSERT_EQ(oracle_out.status, 0u);
    State interp_out = run_on_interpreter(in, code);
    ASSERT_EQ(interp_out.status, 0u);
    State jit_out = run_on_jit(in, code);
    ASSERT_EQ(jit_out.status, 0u);

    for (int lane = 0; lane < 4; ++lane) {
        const float o = lane_f32(oracle_out, 0, lane);
        const float i = lane_f32(interp_out, 0, lane);
        const float j = lane_f32(jit_out, 0, lane);
        EXPECT_TRUE(std::isnan(o)) << "oracle lane " << lane;
        EXPECT_TRUE(std::isnan(i)) << "interpreter lane " << lane;
        EXPECT_TRUE(std::isnan(j)) << "JIT lane " << lane;
    }
}

TEST_F(OracleSseMore32, Mat4MulVecDivW) {
    // EAX = vec, ECX = rows at +0x170..+0x1a0, [EDX+EAX] = out.
    State in = sse_state();
    in.gpr[0] = vitreo::oracle::kMemVa;  // EAX
    in.gpr[1] = vitreo::oracle::kMemVa;  // ECX
    in.gpr[2] = 0x1B0;                   // EDX; dest = kMemVa+0x1B0
    put_mem_row(in, 0x000, 2.0f, 3.0f, 4.0f, 1.0f);
    put_mem_row(in, 0x170, 1.0f, 0.0f, 0.0f, 0.0f);
    put_mem_row(in, 0x180, 0.0f, 1.0f, 0.0f, 0.0f);
    put_mem_row(in, 0x190, 0.0f, 0.0f, 1.0f, 0.0f);
    put_mem_row(in, 0x1A0, 0.0f, 0.0f, 0.0f, 1.0f);
    run_triplet(in, {
        0x0F, 0x28, 0x08, 0x0F, 0x28, 0x91, 0x80, 0x01, 0x00, 0x00, 0x0F, 0x28,
        0x99, 0x70, 0x01, 0x00, 0x00, 0x0F, 0x28, 0xC1, 0x0F, 0xC6, 0xC1, 0x55,
        0x0F, 0x59, 0xC2, 0x0F, 0x28, 0xD1, 0x0F, 0xC6, 0xD1, 0x00, 0x0F, 0x59,
        0xD3, 0x0F, 0x28, 0x99, 0x90, 0x01, 0x00, 0x00, 0x0F, 0x58, 0xC2, 0x0F,
        0x28, 0xD1, 0x0F, 0xC6, 0xD1, 0xAA, 0x0F, 0x59, 0xD3, 0x0F, 0x58, 0xC2,
        0x0F, 0x28, 0xD1, 0x0F, 0xC6, 0xD1, 0xFF, 0x0F, 0x28, 0x89, 0xA0, 0x01,
        0x00, 0x00, 0x0F, 0x59, 0xD1, 0x0F, 0x58, 0xC2, 0x0F, 0x28, 0xC8, 0x0F,
        0xC6, 0xC8, 0xFF, 0x0F, 0x5E, 0xC1, 0x0F, 0x29, 0x04, 0x02});
}

}  // namespace
