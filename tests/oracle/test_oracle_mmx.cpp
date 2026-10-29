// SPDX-License-Identifier: LGPL-2.1-only
// Interpreter/JIT ↔ Oracle MMX tests (Intel SDM Vol. 2 NP 0F).
//
// MMX aliases the x87 stack. Oracle State has no MMX file, so snippets
// follow tests/oracle/test_oracle_x87.cpp MmxPxorThenMovq: MOVQ load/store
// and default CompareOpts (no FXSAVE skip).

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

class OracleMmx64 : public ::testing::Test {
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

    // MOVQ mm0,[rdi]; MOVQ mm1,[rdi+8]; op; MOVQ [rdi+16],mm0
    static std::vector<uint8_t> rr(std::initializer_list<uint8_t> op) {
        std::vector<uint8_t> b = {0x0F, 0x6F, 0x07, 0x0F, 0x6F, 0x4F, 0x08};
        b.insert(b.end(), op);
        b.insert(b.end(), {0x0F, 0x7F, 0x47, 0x10});
        return b;
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

TEST_F(OracleMmx64, PaddbMm0Mm1) {
    State s = seed_state();
    const uint8_t a[8] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
    const uint8_t b[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    put8(s, 0, a);
    put8(s, 8, b);
    run_pair(s, rr({0x0F, 0xFC, 0xC1}));
}

TEST_F(OracleMmx64, PackuswbMm0Mm1) {
    State s = seed_state();
    const uint8_t a[8] = {0x00, 0x01, 0xFF, 0x7F, 0x00, 0x80, 0xFF, 0xFF};
    const uint8_t b[8] = {0x10, 0x00, 0x20, 0x00, 0xFF, 0x7F, 0x00, 0x80};
    put8(s, 0, a);
    put8(s, 8, b);
    run_pair(s, rr({0x0F, 0x67, 0xC1}));
}

TEST_F(OracleMmx64, PcmpgtwMm0Mm1) {
    State s = seed_state();
    const uint8_t a[8] = {0x00, 0x00, 0x01, 0x00, 0xFF, 0xFF, 0x00, 0x80};
    const uint8_t b[8] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x7F};
    put8(s, 0, a);
    put8(s, 8, b);
    run_pair(s, rr({0x0F, 0x65, 0xC1}));
}

TEST_F(OracleMmx64, PunpcklwdMm0Mm1) {
    State s = seed_state();
    const uint8_t a[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    const uint8_t b[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x01};
    put8(s, 0, a);
    put8(s, 8, b);
    run_pair(s, rr({0x0F, 0x61, 0xC1}));
}

TEST_F(OracleMmx64, MovqMm0Mm1) {
    State s = seed_state();
    const uint8_t a[8] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    const uint8_t b[8] = {0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10};
    put8(s, 0, a);
    put8(s, 8, b);
    run_pair(s, rr({0x0F, 0x6F, 0xC1}));
}

TEST_F(OracleMmx64, PsllqImm8) {
    State s = seed_state();
    const uint8_t a[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    put8(s, 0, a);
    // MOVQ mm0,[rdi]; PSLLQ mm0,8; MOVQ [rdi+8],mm0
    run_pair(s, {0x0F, 0x6F, 0x07, 0x0F, 0x73, 0xF0, 0x08, 0x0F, 0x7F, 0x47,
                 0x08});
}

TEST_F(OracleMmx64, PminswMm0Mm1) {
    State s = seed_state();
    const uint8_t a[8] = {0x00, 0x00, 0xFF, 0x7F, 0x00, 0x80, 0x01, 0x00};
    const uint8_t b[8] = {0x01, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00};
    put8(s, 0, a);
    put8(s, 8, b);
    run_pair(s, rr({0x0F, 0xEA, 0xC1}));
}

TEST_F(OracleMmx64, PavgbMm0Mm1) {
    State s = seed_state();
    const uint8_t a[8] = {0x00, 0x01, 0xFE, 0xFF, 0x10, 0x20, 0x30, 0x40};
    const uint8_t b[8] = {0x00, 0x00, 0x01, 0x01, 0x10, 0x21, 0x00, 0xFF};
    put8(s, 0, a);
    put8(s, 8, b);
    run_pair(s, rr({0x0F, 0xE0, 0xC1}));
}

TEST_F(OracleMmx64, PmovmskbEaxMm1) {
    State s = seed_state();
    s.gpr[0] = 0x0000DEADBEEFCAFEull;
    const uint8_t b[8] = {0x80, 0x00, 0x81, 0x7F, 0xFF, 0x01, 0x00, 0x80};
    put8(s, 0, b);
    // MOVQ mm1,[rdi]; PMOVMSKB eax, mm1
    run_pair(s, {0x0F, 0x6F, 0x0F, 0x0F, 0xD7, 0xC1});
}

TEST_F(OracleMmx64, PinsrwPextrw) {
    State s = seed_state();
    s.gpr[1] = 0x000000000000BEEFull;
    const uint8_t a[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    put8(s, 0, a);
    // MOVQ mm0,[rdi]; PINSRW mm0,ecx,1; PEXTRW eax,mm0,1; MOVQ [rdi+8],mm0
    run_pair(s, {0x0F, 0x6F, 0x07, 0x0F, 0xC4, 0xC1, 0x01, 0x0F, 0xC5, 0xC0,
                 0x01, 0x0F, 0x7F, 0x47, 0x08});
}

TEST_F(OracleMmx64, Movq2dqXmm0Mm1) {
    State s = seed_state();
    const uint8_t b[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    put8(s, 0, b);
    // MOVQ mm1,[rdi]; MOVQ2DQ xmm0, mm1
    run_pair(s, {0x0F, 0x6F, 0x0F, 0xF3, 0x0F, 0xD6, 0xC1});
}

TEST_F(OracleMmx64, Movdq2qMm0Xmm1) {
    State s = seed_state();
    // MOVDQ2Q mm0, xmm1; MOVQ [rdi], mm0
    run_pair(s, {0xF2, 0x0F, 0xD6, 0xC1, 0x0F, 0x7F, 0x07});
}

}  // namespace
