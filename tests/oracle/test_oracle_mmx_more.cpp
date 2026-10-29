// SPDX-License-Identifier: LGPL-2.1-only
// Leftover MMX / SSE-MMX / UD2 / ENDBR ↔ Oracle (Intel SDM Vol. 2).

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

class OracleMmxMore64 : public ::testing::Test {
protected:
    void SetUp() override {
        if (!vitreo::oracle::available()) {
            GTEST_SKIP() << "oracle runner not available (requires the host x86 runner)";
        }
    }

    static State seed_state() {
        State s{};
        s.mode = vitreo::oracle::kMode64;
        s.rflags = 0x202;
        s.mxcsr = 0x1F80;
        s.rip = vitreo::oracle::kCodeVa64;
        s.mem_va = vitreo::oracle::kMemVa;
        s.gpr[7] = vitreo::oracle::kMemVa;
        for (int r = 0; r < 16; ++r) {
            for (int b = 0; b < 16; ++b) {
                s.xmm[r][b] = static_cast<uint8_t>(0x10 * r + b);
            }
        }
        return s;
    }

    static void put8(State& s, size_t off, const uint8_t* p) {
        std::memcpy(s.mem.data() + off, p, 8);
    }

    static void run_pair(const State& in, const std::vector<uint8_t>& code,
                         CompareOpts opts = {}) {
        // Rosetta does not model the MMX -> x87 aliasing bookkeeping: an MMX
        // write must set FTW to all-valid and stamp the written register's
        // exponent field to all ones. Verified on real silicon (Windows,
        // 2026-08-30) that hardware does both and Vitreo matches it byte for
        // byte, while Rosetta leaves FTW at its incoming value. See
        // CompareOpts::skip_x87_tags_and_regs. The VALUE side of every MMX op
        // is still fully adjudicated -- these snippets store the result to
        // memory and the memory compare is the contract.
        opts.skip_x87_tags_and_regs = true;
        State oracle_out{};
        ASSERT_TRUE(vitreo::oracle::execute(in, code, &oracle_out))
            << "oracle execution failed";
        ASSERT_EQ(oracle_out.status, 0u) << "oracle snippet faulted";

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

// PSHUFW mm0, mm1, 0x1B — reverse packed words. NP 0F 70 C1 1B.
TEST_F(OracleMmxMore64, PshufwMm0Mm1) {
    State s = seed_state();
    const uint8_t a[8] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
    const uint8_t b[8] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
    put8(s, 0, a);
    put8(s, 8, b);
    run_pair(s, {0x0F, 0x6F, 0x07, 0x0F, 0x6F, 0x4F, 0x08, 0x0F, 0x70, 0xC1,
                 0x1B, 0x0F, 0x7F, 0x47, 0x10});
}

// MASKMOVQ mm0, mm1 — store selected bytes of mm0 to [rdi].
TEST_F(OracleMmxMore64, MaskmovqMm0Mm1) {
    State s = seed_state();
    const uint8_t data[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    const uint8_t mask[8] = {0x80, 0x00, 0x80, 0x00, 0x80, 0x00, 0x80, 0x00};
    put8(s, 0, data);
    put8(s, 8, mask);
    std::memset(s.mem.data() + 16, 0xCD, 8);
    run_pair(s, {0x0F, 0x6F, 0x07, 0x0F, 0x6F, 0x4F, 0x08, 0x0F, 0xF7, 0xC1});
}

// MOVNTQ [rdi], mm0.
TEST_F(OracleMmxMore64, MovntqMemMm0) {
    State s = seed_state();
    const uint8_t src[8] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    put8(s, 8, src);
    run_pair(s, {0x0F, 0x6F, 0x47, 0x08, 0x0F, 0xE7, 0x07});
}

// CVTPI2PS xmm0, mm1 — two i32 → two f32 in low xmm0.
TEST_F(OracleMmxMore64, Cvtpi2psXmm0Mm1) {
    State s = seed_state();
    const uint8_t src[8] = {0x02, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00};
    put8(s, 0, src);
    run_pair(s, {0x0F, 0x6F, 0x0F, 0x0F, 0x2A, 0xC1});
}

// CVTTPS2PI / CVTPS2PI mm0, xmm1 — exact f32 so MXCSR.PE stays clear.
TEST_F(OracleMmxMore64, Cvttps2piMm0Xmm1) {
    State s = seed_state();
    const float f0 = 1.0f;
    const float f1 = 2.0f;
    std::memcpy(s.xmm[1].data(), &f0, 4);
    std::memcpy(s.xmm[1].data() + 4, &f1, 4);
    run_pair(s, {0x0F, 0x2C, 0xC1, 0x0F, 0x7F, 0x07});
}

TEST_F(OracleMmxMore64, Cvtps2piMm0Xmm1) {
    State s = seed_state();
    const float f0 = 1.0f;
    const float f1 = 2.0f;
    std::memcpy(s.xmm[1].data(), &f0, 4);
    std::memcpy(s.xmm[1].data() + 4, &f1, 4);
    run_pair(s, {0x0F, 0x2D, 0xC1, 0x0F, 0x7F, 0x07});
}

// CVTPI2PD xmm0, mm1 — 66 0F 2A C1. Two i32 → two f64.
TEST_F(OracleMmxMore64, Cvtpi2pdXmm0Mm1) {
    State s = seed_state();
    const uint8_t src[8] = {0x02, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00};
    put8(s, 0, src);
    run_pair(s, {0x0F, 0x6F, 0x0F, 0x66, 0x0F, 0x2A, 0xC1});
}

// CVTTPD2PI mm0, xmm1 — 66 0F 2C C1. Exact f64 so MXCSR.PE stays clear.
TEST_F(OracleMmxMore64, Cvttpd2piMm0Xmm1) {
    State s = seed_state();
    const double d0 = 1.0;
    const double d1 = 2.0;
    std::memcpy(s.xmm[1].data(), &d0, 8);
    std::memcpy(s.xmm[1].data() + 8, &d1, 8);
    run_pair(s, {0x66, 0x0F, 0x2C, 0xC1, 0x0F, 0x7F, 0x07});
}

// UD2: both sides raise #UD (status != 0).
TEST_F(OracleMmxMore64, Ud2BothFault) {
    State s = seed_state();
    const std::vector<uint8_t> code = {0x0F, 0x0B};
    State oracle_out{};
    ASSERT_TRUE(vitreo::oracle::execute(s, code, &oracle_out));
    EXPECT_NE(oracle_out.status, 0u);
    State interp_out = run_on_interpreter_64(s, code);
    EXPECT_NE(interp_out.status, 0u);
}

// ENDBR64 is NOP-like when CET is not enabled.
TEST_F(OracleMmxMore64, Endbr64IsNop) {
    State s = seed_state();
    s.gpr[0] = 0x1122334455667788ull;
    run_pair(s, {0xF3, 0x0F, 0x1E, 0xFA});
}

}  // namespace
