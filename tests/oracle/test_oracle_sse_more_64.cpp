// SPDX-License-Identifier: LGPL-2.1-only
// Remaining 64-bit SSE/SSE2/SSE3/SSSE3/SSE4.1 oracle triplets.
// Fixture is OracleSseMore64 so it does not collide with OracleContract64.

#include "oracle/oracle_client.hpp"
#include "oracle/oracle_contract.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

using vitreo::oracle::State;
using vitreo::oracle::contract::compare;
using vitreo::oracle::contract::CompareOpts;
using vitreo::oracle::contract::run_on_interpreter_64;
using vitreo::oracle::contract::run_on_jit_64;

static void put_f32_lane64(State& s, int xmm, int lane, float v) {
    std::memcpy(s.xmm[xmm].data() + lane * 4, &v, 4);
}

static void put_f64_lane64(State& s, int xmm, int lane, double v) {
    std::memcpy(s.xmm[xmm].data() + lane * 8, &v, 8);
}

static void fill_ps(State& s, int xmm, float a, float b, float c, float d) {
    put_f32_lane64(s, xmm, 0, a);
    put_f32_lane64(s, xmm, 1, b);
    put_f32_lane64(s, xmm, 2, c);
    put_f32_lane64(s, xmm, 3, d);
}

static void fill_pd(State& s, int xmm, double a, double b) {
    put_f64_lane64(s, xmm, 0, a);
    put_f64_lane64(s, xmm, 1, b);
}

// Intel SDM Vol. 2.2.1.6: EA = next RIP + disp32. Snippet is at kCodeVa64.
static uint32_t rip_disp(uint64_t target, size_t insn_len) {
    const uint64_t next = vitreo::oracle::kCodeVa64 + insn_len;
    return static_cast<uint32_t>(target - next);
}

// 66 0F <op> 05 disp32 — xmm0, [rip+disp] resolving to kMemVa (8 bytes).
static std::vector<uint8_t> riprel_xmm0_m128(uint8_t opcode) {
    std::vector<uint8_t> code = {0x66, 0x0F, opcode, 0x05, 0, 0, 0, 0};
    const uint32_t d = rip_disp(vitreo::oracle::kMemVa, code.size());
    std::memcpy(code.data() + 4, &d, 4);
    return code;
}

static void put_pd_mem(State& s, double a, double b) {
    std::memcpy(s.mem.data(), &a, 8);
    std::memcpy(s.mem.data() + 8, &b, 8);
}

class OracleSseMore64 : public ::testing::Test {
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

    static void run_triplet(const State& in, const std::vector<uint8_t>& code,
                            uint32_t flags_mask) {
        CompareOpts opts;
        opts.flags_mask = flags_mask;
        run_triplet(in, code, opts);
    }

    static State ps_state() {
        State s = seed_state();
        fill_ps(s, 0, 1.0f, 2.0f, 3.0f, 4.0f);
        fill_ps(s, 1, 5.0f, 6.0f, 7.0f, 8.0f);
        return s;
    }

    static State pd_state() {
        State s = seed_state();
        fill_pd(s, 0, 1.0, 2.0);
        fill_pd(s, 1, 5.0, 6.0);
        return s;
    }
};

// --- packed / scalar FP ------------------------------------------------------

TEST_F(OracleSseMore64, Movaps) {
    run_triplet(seed_state(), {0x0F, 0x28, 0xC1});  // movaps xmm0, xmm1
}

TEST_F(OracleSseMore64, Movapd) {
    run_triplet(seed_state(), {0x66, 0x0F, 0x28, 0xC1});  // movapd xmm0, xmm1
}

TEST_F(OracleSseMore64, Movups) {
    run_triplet(seed_state(), {0x0F, 0x10, 0xC1});  // movups xmm0, xmm1
}

TEST_F(OracleSseMore64, Movdqu) {
    run_triplet(seed_state(), {0xF3, 0x0F, 0x6F, 0xC1});  // movdqu xmm0, xmm1
}

TEST_F(OracleSseMore64, Xorps) {
    run_triplet(seed_state(), {0x0F, 0x57, 0xC1});  // xorps xmm0, xmm1
}

TEST_F(OracleSseMore64, Xorpd) {
    run_triplet(seed_state(), {0x66, 0x0F, 0x57, 0xC1});  // xorpd xmm0, xmm1
}

TEST_F(OracleSseMore64, Orps) {
    run_triplet(seed_state(), {0x0F, 0x56, 0xC1});  // orps xmm0, xmm1
}

TEST_F(OracleSseMore64, Andnps) {
    run_triplet(seed_state(), {0x0F, 0x55, 0xC1});  // andnps xmm0, xmm1
}

TEST_F(OracleSseMore64, Subps) {
    run_triplet(ps_state(), {0x0F, 0x5C, 0xC1});  // subps xmm0, xmm1
}

TEST_F(OracleSseMore64, Mulps) {
    run_triplet(ps_state(), {0x0F, 0x59, 0xC1});  // mulps xmm0, xmm1
}

TEST_F(OracleSseMore64, Divps) {
    State s = seed_state();
    fill_ps(s, 0, 8.0f, 18.0f, 32.0f, 50.0f);
    fill_ps(s, 1, 2.0f, 3.0f, 4.0f, 5.0f);
    run_triplet(s, {0x0F, 0x5E, 0xC1});  // divps xmm0, xmm1
}

TEST_F(OracleSseMore64, DivpsInexactSetsPe) {
    State s = seed_state();
    fill_ps(s, 0, 1.0f, 1.0f, 1.0f, 1.0f);
    fill_ps(s, 1, 3.0f, 3.0f, 3.0f, 3.0f);
    run_triplet(s, {0x0F, 0x5E, 0xC1});
}

TEST_F(OracleSseMore64, Minps) {
    run_triplet(ps_state(), {0x0F, 0x5D, 0xC1});  // minps xmm0, xmm1
}

TEST_F(OracleSseMore64, Maxps) {
    run_triplet(ps_state(), {0x0F, 0x5F, 0xC1});  // maxps xmm0, xmm1
}

TEST_F(OracleSseMore64, Sqrtps) {
    State s = seed_state();
    fill_ps(s, 1, 1.0f, 4.0f, 9.0f, 16.0f);
    run_triplet(s, {0x0F, 0x51, 0xC1});  // sqrtps xmm0, xmm1
}

TEST_F(OracleSseMore64, Addpd) {
    run_triplet(pd_state(), {0x66, 0x0F, 0x58, 0xC1});  // addpd xmm0, xmm1
}

TEST_F(OracleSseMore64, Subpd) {
    run_triplet(pd_state(), {0x66, 0x0F, 0x5C, 0xC1});  // subpd xmm0, xmm1
}

TEST_F(OracleSseMore64, AddpdRipRel) {
    State s = pd_state();
    put_pd_mem(s, 5.0, 6.0);
    run_triplet(s, riprel_xmm0_m128(0x58));  // addpd xmm0, [rip+disp]
}

TEST_F(OracleSseMore64, SubpdRipRel) {
    State s = pd_state();
    put_pd_mem(s, 5.0, 6.0);
    run_triplet(s, riprel_xmm0_m128(0x5C));  // subpd xmm0, [rip+disp]
}

TEST_F(OracleSseMore64, MulpdRipRel) {
    State s = pd_state();
    put_pd_mem(s, 5.0, 6.0);
    run_triplet(s, riprel_xmm0_m128(0x59));  // mulpd xmm0, [rip+disp]
}

TEST_F(OracleSseMore64, Mulpd) {
    run_triplet(pd_state(), {0x66, 0x0F, 0x59, 0xC1});  // mulpd xmm0, xmm1
}

TEST_F(OracleSseMore64, Addss) {
    State s = seed_state();
    put_f32_lane64(s, 0, 0, 1.0f);
    put_f32_lane64(s, 1, 0, 5.0f);
    run_triplet(s, {0xF3, 0x0F, 0x58, 0xC1});  // addss xmm0, xmm1
}

TEST_F(OracleSseMore64, Addsd) {
    State s = seed_state();
    put_f64_lane64(s, 0, 0, 1.0);
    put_f64_lane64(s, 1, 0, 5.0);
    run_triplet(s, {0xF2, 0x0F, 0x58, 0xC1});  // addsd xmm0, xmm1
}

TEST_F(OracleSseMore64, Sqrtss) {
    State s = seed_state();
    put_f32_lane64(s, 1, 0, 4.0f);
    run_triplet(s, {0xF3, 0x0F, 0x51, 0xC1});  // sqrtss xmm0, xmm1
}

TEST_F(OracleSseMore64, Cvtps2pd) {
    State s = seed_state();
    put_f32_lane64(s, 1, 0, 1.0f);
    put_f32_lane64(s, 1, 1, 2.0f);
    run_triplet(s, {0x0F, 0x5A, 0xC1});  // cvtps2pd xmm0, xmm1
}

TEST_F(OracleSseMore64, Addsubps) {
    run_triplet(ps_state(), {0xF2, 0x0F, 0xD0, 0xC1});  // addsubps xmm0, xmm1
}

TEST_F(OracleSseMore64, Hsubps) {
    run_triplet(ps_state(), {0xF2, 0x0F, 0x7D, 0xC1});  // hsubps xmm0, xmm1
}

TEST_F(OracleSseMore64, CmppsEq) {
    State s = ps_state();
    fill_ps(s, 1, 1.0f, 6.0f, 3.0f, 8.0f);
    run_triplet(s, {0x0F, 0xC2, 0xC1, 0x00});  // cmpps xmm0, xmm1, eq
}

TEST_F(OracleSseMore64, Ucomiss) {
    State s = seed_state();
    put_f32_lane64(s, 0, 0, 1.0f);
    put_f32_lane64(s, 1, 0, 2.0f);
    run_triplet(s, {0x0F, 0x2E, 0xC1});  // ucomiss xmm0, xmm1
}

TEST_F(OracleSseMore64, Comisd) {
    State s = seed_state();
    put_f64_lane64(s, 0, 0, 1.0);
    put_f64_lane64(s, 1, 0, 2.0);
    run_triplet(s, {0x66, 0x0F, 0x2F, 0xC1});  // comisd xmm0, xmm1
}

TEST_F(OracleSseMore64, Ucomisd) {
    State s = seed_state();
    put_f64_lane64(s, 0, 0, 1.0);
    put_f64_lane64(s, 1, 0, 2.0);
    run_triplet(s, {0x66, 0x0F, 0x2E, 0xC1});  // ucomisd xmm0, xmm1
}

TEST_F(OracleSseMore64, Roundpd) {
    State s = seed_state();
    fill_pd(s, 1, 1.5, 2.5);
    s.xmm[0].fill(0);
    run_triplet(s, {0x66, 0x0F, 0x3A, 0x09, 0xC1, 0x00});  // roundpd xmm0, xmm1, 0
}

TEST_F(OracleSseMore64, Cvtsi2sdRax) {
    State s = seed_state();
    s.gpr[0] = 42;
    run_triplet(s, {0xF2, 0x48, 0x0F, 0x2A, 0xC0});  // cvtsi2sd xmm0, rax
}

TEST_F(OracleSseMore64, Cvtsi2ssRax) {
    State s = seed_state();
    s.gpr[0] = 42;
    run_triplet(s, {0xF3, 0x48, 0x0F, 0x2A, 0xC0});  // cvtsi2ss xmm0, rax
}

// --- moves / extracts / inserts ---------------------------------------------

TEST_F(OracleSseMore64, MovdXmmEax) {
    run_triplet(seed_state(), {0x66, 0x0F, 0x6E, 0xC0});  // movd xmm0, eax
}

TEST_F(OracleSseMore64, MovdEaxXmm) {
    run_triplet(seed_state(), {0x66, 0x0F, 0x7E, 0xC0});  // movd eax, xmm0
}

TEST_F(OracleSseMore64, MovqXmmXmm) {
    run_triplet(seed_state(), {0xF3, 0x0F, 0x7E, 0xC1});  // movq xmm0, xmm1
}

TEST_F(OracleSseMore64, MovqXmmRax) {
    run_triplet(seed_state(), {0x66, 0x48, 0x0F, 0x6E, 0xC0});  // movq xmm0, rax
}

TEST_F(OracleSseMore64, MovqRaxXmm) {
    run_triplet(seed_state(), {0x66, 0x48, 0x0F, 0x7E, 0xC0});  // movq rax, xmm0
}

TEST_F(OracleSseMore64, Movss) {
    run_triplet(seed_state(), {0xF3, 0x0F, 0x10, 0xC1});  // movss xmm0, xmm1
}

TEST_F(OracleSseMore64, Movsldup) {
    run_triplet(ps_state(), {0xF3, 0x0F, 0x12, 0xC1});  // movsldup xmm0, xmm1
}

TEST_F(OracleSseMore64, Movddup) {
    run_triplet(pd_state(), {0xF2, 0x0F, 0x12, 0xC1});  // movddup xmm0, xmm1
}

TEST_F(OracleSseMore64, MovlpsFromRdi) {
    run_triplet(seed_state(), {0x0F, 0x12, 0x07});  // movlps xmm0, [rdi]
}

TEST_F(OracleSseMore64, MovlpsToRdi) {
    run_triplet(seed_state(), {0x0F, 0x13, 0x07});  // movlps [rdi], xmm0
}

TEST_F(OracleSseMore64, MovhpsFromRdi) {
    run_triplet(seed_state(), {0x0F, 0x16, 0x07});  // movhps xmm0, [rdi]
}

TEST_F(OracleSseMore64, MovhpsToRdi) {
    run_triplet(seed_state(), {0x0F, 0x17, 0x07});  // movhps [rdi], xmm0
}

TEST_F(OracleSseMore64, MovlpdFromRdi) {
    run_triplet(seed_state(), {0x66, 0x0F, 0x12, 0x07});  // movlpd xmm0, [rdi]
}

TEST_F(OracleSseMore64, MovlpdToRdi) {
    run_triplet(seed_state(), {0x66, 0x0F, 0x13, 0x07});  // movlpd [rdi], xmm0
}

TEST_F(OracleSseMore64, MovhpdFromRdi) {
    run_triplet(seed_state(), {0x66, 0x0F, 0x16, 0x07});  // movhpd xmm0, [rdi]
}

TEST_F(OracleSseMore64, MovhpdToRdi) {
    run_triplet(seed_state(), {0x66, 0x0F, 0x17, 0x07});  // movhpd [rdi], xmm0
}

TEST_F(OracleSseMore64, LddquFromRdi) {
    run_triplet(seed_state(), {0xF2, 0x0F, 0xF0, 0x07});  // lddqu xmm0, [rdi]
}

// Intel SDM Vol. 2 RCPSS/RSQRTSS: DEST[31:0] := APPROXIMATE(...);
// DEST[127:32] unmodified; SIMD FP exceptions: None. Vitreo uses exact
// 1/x and 1/sqrt(x) (within the 1.5*2^-12 bound). Rosetta may differ in
// lane-0 low bits and may not set MXCSR.PE the same way. Mask those;
// do not force a bit-exact fake match.
static constexpr uint32_t kMxcsrPe = 1u << 5;

static void mask_approx_ss(State& s) {
    std::memset(s.xmm[0].data(), 0, 4);
    s.mxcsr &= ~kMxcsrPe;
}

static void run_approx_ss(const State& in, const std::vector<uint8_t>& code,
                          float src, bool rsqrt) {
    State oracle_out{};
    ASSERT_TRUE(vitreo::oracle::execute(in, code, &oracle_out))
        << "oracle execution failed";
    ASSERT_EQ(oracle_out.status, 0u) << "oracle snippet faulted";
    ASSERT_EQ(oracle_out.rip, vitreo::oracle::kCodeVa64);

    State interp_out = run_on_interpreter_64(in, code);
    ASSERT_EQ(interp_out.status, 0u) << "interpreter snippet faulted";
    {
        float got = 0.0f;
        std::memcpy(&got, interp_out.xmm[0].data(), 4);
        const float exact = rsqrt ? (1.0f / std::sqrt(src)) : (1.0f / src);
        const float rel = std::fabs(got - exact) / std::fabs(exact);
        EXPECT_LE(rel, 1.5f * (1.0f / 4096.0f))
            << "interpreter lane0 outside SDM RCPSS/RSQRTSS bound";
    }
    mask_approx_ss(interp_out);
    mask_approx_ss(oracle_out);
    std::string interp_diff = compare(interp_out, oracle_out);
    EXPECT_TRUE(interp_diff.empty())
        << "interpreter ↔ oracle divergence (lane0/PE masked):\n"
        << interp_diff;

    State jit_out = run_on_jit_64(in, code);
    if (jit_out.status == 7u) {
        GTEST_SKIP() << "JIT has no native lowering for this snippet";
    }
    ASSERT_EQ(jit_out.status, 0u) << "JIT snippet faulted";
    mask_approx_ss(jit_out);
    std::string jit_diff = compare(jit_out, oracle_out);
    EXPECT_TRUE(jit_diff.empty())
        << "JIT ↔ oracle divergence (lane0/PE masked):\n" << jit_diff;
}

TEST_F(OracleSseMore64, Rcpss) {
    State s = seed_state();
    put_f32_lane64(s, 1, 0, 4.0f);
    run_approx_ss(s, {0xF3, 0x0F, 0x53, 0xC1}, 4.0f, false);  // rcpss xmm0, xmm1
}

TEST_F(OracleSseMore64, Rsqrtss) {
    State s = seed_state();
    put_f32_lane64(s, 1, 0, 4.0f);
    run_approx_ss(s, {0xF3, 0x0F, 0x52, 0xC1}, 4.0f, true);  // rsqrtss xmm0, xmm1
}

TEST_F(OracleSseMore64, RcpssFromRdi) {
    State s = seed_state();
    const float src = 4.0f;
    std::memcpy(s.mem.data(), &src, 4);
    run_approx_ss(s, {0xF3, 0x0F, 0x53, 0x07}, src, false);  // rcpss xmm0, [rdi]
}

TEST_F(OracleSseMore64, RsqrtssFromRdi) {
    State s = seed_state();
    const float src = 4.0f;
    std::memcpy(s.mem.data(), &src, 4);
    run_approx_ss(s, {0xF3, 0x0F, 0x52, 0x07}, src, true);  // rsqrtss xmm0, [rdi]
}

TEST_F(OracleSseMore64, Blendvps) {
    State s = seed_state();
    s.xmm[0] = {0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00};
    s.xmm[1].fill(0x11);
    s.xmm[2].fill(0xAA);
    run_triplet(s, {0x66, 0x0F, 0x38, 0x14, 0xCA});  // blendvps xmm1, xmm2
}

TEST_F(OracleSseMore64, Blendvpd) {
    State s = seed_state();
    s.xmm[0] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    s.xmm[1].fill(0x11);
    s.xmm[2].fill(0xAA);
    run_triplet(s, {0x66, 0x0F, 0x38, 0x15, 0xCA});  // blendvpd xmm1, xmm2
}

TEST_F(OracleSseMore64, Pmovmskb) {
    run_triplet(seed_state(), {0x66, 0x0F, 0xD7, 0xC0});  // pmovmskb eax, xmm0
}

TEST_F(OracleSseMore64, Movmskps) {
    run_triplet(ps_state(), {0x0F, 0x50, 0xC0});  // movmskps eax, xmm0
}

TEST_F(OracleSseMore64, Extractps) {
    run_triplet(ps_state(), {0x66, 0x0F, 0x3A, 0x17, 0xC0, 0x01});  // extractps eax, xmm0, 1
}

TEST_F(OracleSseMore64, Insertps) {
    State s = ps_state();
    run_triplet(s, {0x66, 0x0F, 0x3A, 0x21, 0xC1, 0x40});  // insertps xmm0, xmm1, 0x40
}

TEST_F(OracleSseMore64, Pinsrw) {
    State s = seed_state();
    s.gpr[0] = 0xAABBCCDDull;
    run_triplet(s, {0x66, 0x0F, 0xC4, 0xC0, 0x01});  // pinsrw xmm0, eax, 1
}

TEST_F(OracleSseMore64, Pextrw) {
    run_triplet(seed_state(), {0x66, 0x0F, 0xC5, 0xC0, 0x01});  // pextrw eax, xmm0, 1
}

TEST_F(OracleSseMore64, Pinsrb) {
    State s = seed_state();
    s.gpr[0] = 0xAABBCCDDull;
    run_triplet(s, {0x66, 0x0F, 0x3A, 0x20, 0xC0, 0x05});  // pinsrb xmm0, eax, 5
}

TEST_F(OracleSseMore64, Pextrb) {
    run_triplet(seed_state(), {0x66, 0x0F, 0x3A, 0x14, 0xC0, 0x05});  // pextrb eax, xmm0, 5
}

TEST_F(OracleSseMore64, Pinsrq) {
    State s = seed_state();
    s.gpr[0] = 0x0123456789ABCDEFULL;
    run_triplet(s, {0x66, 0x48, 0x0F, 0x3A, 0x22, 0xC0, 0x01});  // pinsrq xmm0, rax, 1
}

TEST_F(OracleSseMore64, Pextrq) {
    run_triplet(seed_state(), {0x66, 0x48, 0x0F, 0x3A, 0x16, 0xC0, 0x01});  // pextrq rax, xmm0, 1
}

TEST_F(OracleSseMore64, PextrqRcxXmm0Imm0) {
    run_triplet(seed_state(), {0x66, 0x48, 0x0F, 0x3A, 0x16, 0xC1, 0x00});
}

TEST_F(OracleSseMore64, PinsrqXmm0RcxImm0) {
    State s = seed_state();
    s.gpr[1] = 0x0123456789ABCDEFULL;
    run_triplet(s, {0x66, 0x48, 0x0F, 0x3A, 0x22, 0xC1, 0x00});
}

TEST_F(OracleSseMore64, PextrdEcxXmm0Imm0) {
    run_triplet(seed_state(), {0x66, 0x0F, 0x3A, 0x16, 0xC1, 0x00});
}

TEST_F(OracleSseMore64, PinsrdXmm0EcxImm0) {
    State s = seed_state();
    s.gpr[1] = 0xAABBCCDDull;
    run_triplet(s, {0x66, 0x0F, 0x3A, 0x22, 0xC1, 0x00});
}

TEST_F(OracleSseMore64, PextrbEcxXmm0Imm0) {
    run_triplet(seed_state(), {0x66, 0x0F, 0x3A, 0x14, 0xC1, 0x00});
}

TEST_F(OracleSseMore64, PinsrbXmm0EcxImm0) {
    State s = seed_state();
    s.gpr[1] = 0xAABBCCDDull;
    run_triplet(s, {0x66, 0x0F, 0x3A, 0x20, 0xC1, 0x00});
}

TEST_F(OracleSseMore64, ExtractpsEcxXmm0Imm0) {
    run_triplet(ps_state(), {0x66, 0x0F, 0x3A, 0x17, 0xC1, 0x00});
}

TEST_F(OracleSseMore64, InsertpsXmm0Xmm1Imm0) {
    run_triplet(ps_state(), {0x66, 0x0F, 0x3A, 0x21, 0xC1, 0x00});
}

TEST_F(OracleSseMore64, PmovmskbRax) {
    run_triplet(seed_state(), {0x66, 0x48, 0x0F, 0xD7, 0xC1});  // pmovmskb rax, xmm1
}

TEST_F(OracleSseMore64, Cvtss2siEax) {
    State s = seed_state();
    put_f32_lane64(s, 1, 0, 42.0f);
    run_triplet(s, {0xF3, 0x0F, 0x2D, 0xC1});  // cvtss2si eax, xmm1
}

// Intel SDM Vol. 2 CVTSS2SI: inexact finite conversion sets MXCSR.PE.
TEST_F(OracleSseMore64, Cvtss2siEaxInexactSetsPe) {
    State s = seed_state();
    put_f32_lane64(s, 1, 0, 1.5f);
    run_triplet(s, {0xF3, 0x0F, 0x2D, 0xC1});
}

TEST_F(OracleSseMore64, Cvtss2siRax) {
    State s = seed_state();
    put_f32_lane64(s, 1, 0, 42.0f);
    run_triplet(s, {0xF3, 0x48, 0x0F, 0x2D, 0xC1});  // cvtss2si rax, xmm1
}

TEST_F(OracleSseMore64, Cvtsd2siEax) {
    State s = seed_state();
    put_f64_lane64(s, 1, 0, 42.0);
    run_triplet(s, {0xF2, 0x0F, 0x2D, 0xC1});
}

TEST_F(OracleSseMore64, Cvtsd2siRax) {
    State s = seed_state();
    put_f64_lane64(s, 1, 0, 42.0);
    run_triplet(s, {0xF2, 0x48, 0x0F, 0x2D, 0xC1});
}

TEST_F(OracleSseMore64, Cvttss2siEax) {
    State s = seed_state();
    put_f32_lane64(s, 1, 0, 4.0f);
    run_triplet(s, {0xF3, 0x0F, 0x2C, 0xC1});
}

TEST_F(OracleSseMore64, Cvttss2siEaxInexactSetsPe) {
    State s = seed_state();
    put_f32_lane64(s, 1, 0, 1.5f);
    run_triplet(s, {0xF3, 0x0F, 0x2C, 0xC1});
}

TEST_F(OracleSseMore64, Cvttss2siRax) {
    State s = seed_state();
    put_f32_lane64(s, 1, 0, 4.0f);
    run_triplet(s, {0xF3, 0x48, 0x0F, 0x2C, 0xC1});
}

TEST_F(OracleSseMore64, Cvttsd2siEax) {
    State s = seed_state();
    put_f64_lane64(s, 1, 0, 4.0);
    run_triplet(s, {0xF2, 0x0F, 0x2C, 0xC1});
}

TEST_F(OracleSseMore64, Cvttsd2siRax) {
    State s = seed_state();
    put_f64_lane64(s, 1, 0, 4.0);
    run_triplet(s, {0xF2, 0x48, 0x0F, 0x2C, 0xC1});
}

TEST_F(OracleSseMore64, Cvtss2sd) {
    State s = seed_state();
    put_f32_lane64(s, 1, 0, 1.5f);
    run_triplet(s, {0xF3, 0x0F, 0x5A, 0xC1});
}

TEST_F(OracleSseMore64, Cvtsd2ss) {
    State s = seed_state();
    put_f64_lane64(s, 1, 0, 1.5);
    run_triplet(s, {0xF2, 0x0F, 0x5A, 0xC1});
}

TEST_F(OracleSseMore64, Cvtsd2ssInexactSetsPe) {
    State s = seed_state();
    put_f64_lane64(s, 1, 0, 1.0 / 3.0);
    run_triplet(s, {0xF2, 0x0F, 0x5A, 0xC1});
}

TEST_F(OracleSseMore64, Cvtps2dq) {
    State s = seed_state();
    fill_ps(s, 1, 1.0f, 2.0f, 3.0f, 4.0f);
    run_triplet(s, {0x66, 0x0F, 0x5B, 0xC1});
}

TEST_F(OracleSseMore64, Cvtps2dqInexactSetsPe) {
    State s = seed_state();
    fill_ps(s, 1, 1.5f, 2.0f, 3.0f, 4.0f);
    run_triplet(s, {0x66, 0x0F, 0x5B, 0xC1});
}

TEST_F(OracleSseMore64, Cvtpd2dq) {
    State s = seed_state();
    fill_pd(s, 1, 1.0, 2.0);
    run_triplet(s, {0xF2, 0x0F, 0xE6, 0xC1});
}

TEST_F(OracleSseMore64, Cvttps2dq) {
    State s = seed_state();
    fill_ps(s, 1, 1.0f, 2.0f, -3.0f, 4.0f);
    run_triplet(s, {0xF3, 0x0F, 0x5B, 0xC1});
}

TEST_F(OracleSseMore64, Cvttpd2dq) {
    State s = seed_state();
    fill_pd(s, 1, 1.0, -2.0);
    run_triplet(s, {0x66, 0x0F, 0xE6, 0xC1});
}

TEST_F(OracleSseMore64, Haddpd) {
    run_triplet(pd_state(), {0x66, 0x0F, 0x7C, 0xC1});
}

TEST_F(OracleSseMore64, Hsubpd) {
    run_triplet(pd_state(), {0x66, 0x0F, 0x7D, 0xC1});
}

TEST_F(OracleSseMore64, Addsubpd) {
    run_triplet(pd_state(), {0x66, 0x0F, 0xD0, 0xC1});
}

TEST_F(OracleSseMore64, Movshdup) {
    run_triplet(ps_state(), {0xF3, 0x0F, 0x16, 0xC1});
}

// --- integer SIMD ------------------------------------------------------------

TEST_F(OracleSseMore64, Por) {
    run_triplet(seed_state(), {0x66, 0x0F, 0xEB, 0xC1});  // por xmm0, xmm1
}

TEST_F(OracleSseMore64, Pandn) {
    run_triplet(seed_state(), {0x66, 0x0F, 0xDF, 0xC1});  // pandn xmm0, xmm1
}

TEST_F(OracleSseMore64, Psubd) {
    run_triplet(seed_state(), {0x66, 0x0F, 0xFA, 0xC1});  // psubd xmm0, xmm1
}

TEST_F(OracleSseMore64, Pmullw) {
    run_triplet(seed_state(), {0x66, 0x0F, 0xD5, 0xC1});  // pmullw xmm0, xmm1
}

TEST_F(OracleSseMore64, Pcmpeqb) {
    run_triplet(seed_state(), {0x66, 0x0F, 0x74, 0xC1});  // pcmpeqb xmm0, xmm1
}

TEST_F(OracleSseMore64, Pcmpgtb) {
    run_triplet(seed_state(), {0x66, 0x0F, 0x64, 0xC1});  // pcmpgtb xmm0, xmm1
}

TEST_F(OracleSseMore64, Pmulld) {
    run_triplet(seed_state(), {0x66, 0x0F, 0x38, 0x40, 0xC1});  // pmulld xmm0, xmm1
}

TEST_F(OracleSseMore64, Pmovzxbw) {
    State s = seed_state();
    s.xmm[1] = {0x7F, 0x80, 0x01, 0xFF, 0x00, 0x40, 0xC0, 0x10,
                0, 0, 0, 0, 0, 0, 0, 0};
    run_triplet(s, {0x66, 0x0F, 0x38, 0x30, 0xC1});  // pmovzxbw xmm0, xmm1
}

TEST_F(OracleSseMore64, Pminub) {
    run_triplet(seed_state(), {0x66, 0x0F, 0xDA, 0xC1});  // pminub xmm0, xmm1
}

TEST_F(OracleSseMore64, Pminsd) {
    run_triplet(seed_state(), {0x66, 0x0F, 0x38, 0x39, 0xC1});  // pminsd xmm0, xmm1
}

TEST_F(OracleSseMore64, Pmaxsd) {
    run_triplet(seed_state(), {0x66, 0x0F, 0x38, 0x3D, 0xC1});  // pmaxsd xmm0, xmm1
}

TEST_F(OracleSseMore64, Pavgb) {
    run_triplet(seed_state(), {0x66, 0x0F, 0xE0, 0xC1});  // pavgb xmm0, xmm1
}

TEST_F(OracleSseMore64, Psadbw) {
    run_triplet(seed_state(), {0x66, 0x0F, 0xF6, 0xC1});  // psadbw xmm0, xmm1
}

TEST_F(OracleSseMore64, Paddusb) {
    run_triplet(seed_state(), {0x66, 0x0F, 0xDC, 0xC1});  // paddusb xmm0, xmm1
}

TEST_F(OracleSseMore64, PsllwImm) {
    run_triplet(seed_state(), {0x66, 0x0F, 0x71, 0xF0, 0x04});  // psllw xmm0, 4
}

TEST_F(OracleSseMore64, Psrldq) {
    run_triplet(seed_state(), {0x66, 0x0F, 0x73, 0xD8, 0x04});  // psrldq xmm0, 4
}

TEST_F(OracleSseMore64, Pshufd) {
    run_triplet(seed_state(), {0x66, 0x0F, 0x70, 0xC1, 0x1B});  // pshufd xmm0, xmm1, 0x1B
}

TEST_F(OracleSseMore64, Pshuflw) {
    run_triplet(seed_state(), {0xF2, 0x0F, 0x70, 0xC1, 0x1B});  // pshuflw xmm0, xmm1, 0x1B
}

TEST_F(OracleSseMore64, Shufpd) {
    run_triplet(pd_state(), {0x66, 0x0F, 0xC6, 0xC1, 0x01});  // shufpd xmm0, xmm1, 1
}

TEST_F(OracleSseMore64, Punpcklbw) {
    run_triplet(seed_state(), {0x66, 0x0F, 0x60, 0xC1});  // punpcklbw xmm0, xmm1
}

TEST_F(OracleSseMore64, Punpckldq) {
    run_triplet(seed_state(), {0x66, 0x0F, 0x62, 0xC1});  // punpckldq xmm0, xmm1
}

TEST_F(OracleSseMore64, PunpckldqRipRel) {
    run_triplet(seed_state(), riprel_xmm0_m128(0x62));  // punpckldq xmm0, [rip+disp]
}

TEST_F(OracleSseMore64, Unpcklps) {
    run_triplet(ps_state(), {0x0F, 0x14, 0xC1});  // unpcklps xmm0, xmm1
}

TEST_F(OracleSseMore64, Pabsb) {
    run_triplet(seed_state(), {0x66, 0x0F, 0x38, 0x1C, 0xC1});  // pabsb xmm0, xmm1
}

TEST_F(OracleSseMore64, Packsswb) {
    run_triplet(seed_state(), {0x66, 0x0F, 0x63, 0xC1});  // packsswb xmm0, xmm1
}

TEST_F(OracleSseMore64, Phaddd) {
    run_triplet(seed_state(), {0x66, 0x0F, 0x38, 0x02, 0xC1});  // phaddd xmm0, xmm1
}

TEST_F(OracleSseMore64, Psignb) {
    run_triplet(seed_state(), {0x66, 0x0F, 0x38, 0x08, 0xC1});  // psignb xmm0, xmm1
}

TEST_F(OracleSseMore64, Blendpd) {
    run_triplet(pd_state(), {0x66, 0x0F, 0x3A, 0x0D, 0xC1, 0x01});  // blendpd xmm0, xmm1, 1
}

TEST_F(OracleSseMore64, Pblendw) {
    run_triplet(seed_state(), {0x66, 0x0F, 0x3A, 0x0E, 0xC1, 0x55});  // pblendw xmm0, xmm1, 0x55
}

// --- XMM8-15 -----------------------------------------------------------------

TEST_F(OracleSseMore64, PadddXmm8Xmm9) {
    run_triplet(seed_state(), {0x66, 0x45, 0x0F, 0xFE, 0xC1});  // paddd xmm8, xmm9
}

TEST_F(OracleSseMore64, PorXmm15Xmm8) {
    run_triplet(seed_state(), {0x66, 0x45, 0x0F, 0xEB, 0xF8});  // por xmm15, xmm8
}

// --- non-temporal / cache-hint stores (mem via [rdi] = kMemVa) ---------------
// Intel SDM Vol. 2 MOVNTDQ / MOVNTPS / MOVNTI: NT hint is not architecturally
// visible; the store bits must match. CLFLUSH is a hint on a valid address.

TEST_F(OracleSseMore64, MovntdqMemXmm0) {
    run_triplet(seed_state(), {0x66, 0x0F, 0xE7, 0x07});  // movntdq [rdi], xmm0
}

TEST_F(OracleSseMore64, MovntpsMemXmm0) {
    run_triplet(seed_state(), {0x0F, 0x2B, 0x07});  // movntps [rdi], xmm0
}

TEST_F(OracleSseMore64, MovntiMemEax) {
    State s = seed_state();
    s.gpr[0] = 0x0000'0000'89AB'CDEFull;
    run_triplet(s, {0x0F, 0xC3, 0x07});  // movnti [rdi], eax
}

TEST_F(OracleSseMore64, MovntiMemRax) {
    State s = seed_state();
    s.gpr[0] = 0x0123'4567'89AB'CDEFull;
    run_triplet(s, {0x48, 0x0F, 0xC3, 0x07});  // movnti [rdi], rax
}

TEST_F(OracleSseMore64, ClflushRdi) {
    run_triplet(seed_state(), {0x0F, 0xAE, 0x3F});  // clflush [rdi]
}

// Intel SDM Vol. 2 MASKMOVDQU: store xmm0 bytes to [RDI] where xmm1 MSB is set.
TEST_F(OracleSseMore64, MaskmovdquXmm0Xmm1) {
    State s = seed_state();
    for (int b = 0; b < 16; ++b) {
        s.xmm[1][static_cast<size_t>(b)] =
            (b & 1) ? static_cast<uint8_t>(0x80) : static_cast<uint8_t>(0x00);
    }
    run_triplet(s, {0x66, 0x0F, 0xF7, 0xC1});
}

}  // namespace
