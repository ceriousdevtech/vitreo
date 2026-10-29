// SPDX-License-Identifier: LGPL-2.1-only
// Oracle plumbing sanity test. Verifies that the oracle runner is reachable,
// executes correctly, and produces expected bit-exact results for a handful of
// canonical x86 snippets. This is *not* a contract test against the Vitreo
// interpreter yet — that lands in test_oracle_contract.cpp once this is green.

#include "oracle/oracle_client.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using vitreo::oracle::State;

class OracleRunnerTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!vitreo::oracle::available()) {
            GTEST_SKIP() << "oracle runner not available (requires the host x86 runner)";
        }
    }
};

TEST_F(OracleRunnerTest, RoundTripsState) {
    State in{};
    in.gpr = {0xAAAAAAAA, 0xCCCCCCCC, 0xDDDDDDDD, 0xBBBBBBBB,
              0, 0x66666666, 0x77777777, 0x88888888};

    State out{};
    ASSERT_TRUE(vitreo::oracle::execute(in, {}, &out));
    EXPECT_EQ(out.status, 0u);
    for (int i = 0; i < 8; ++i) {
        if (i == 4) continue;  // ESP skipped intentionally
        EXPECT_EQ(out.gpr[i], in.gpr[i]) << "GPR " << i;
    }
}

TEST_F(OracleRunnerTest, AddEaxEcxMatches) {
    State in{};
    in.gpr[0] = 0xAAAAAAAAu;  // EAX
    in.gpr[1] = 0xCCCCCCCCu;  // ECX

    State out{};
    ASSERT_TRUE(vitreo::oracle::execute(in, {0x01, 0xC8}, &out));  // ADD EAX, ECX
    ASSERT_EQ(out.status, 0u);

    const uint32_t expected = in.gpr[0] + in.gpr[1];
    EXPECT_EQ(out.gpr[0], expected);
    EXPECT_TRUE(out.rflags & 0x1u) << "CF should be set for overflow";
}

TEST_F(OracleRunnerTest, PxorZerosXmm) {
    State in{};
    for (int i = 0; i < 8; ++i) {
        in.xmm[i].fill(static_cast<uint8_t>(i + 1));
    }

    State out{};
    // PXOR xmm0, xmm0 -> 66 0F EF C0
    ASSERT_TRUE(vitreo::oracle::execute(in, {0x66, 0x0F, 0xEF, 0xC0}, &out));
    ASSERT_EQ(out.status, 0u);

    for (auto b : out.xmm[0]) EXPECT_EQ(b, 0) << "XMM0 should be zero";
    EXPECT_EQ(out.xmm[1], in.xmm[1]) << "XMM1 should be preserved";
}

TEST_F(OracleRunnerTest, PshufbReverseLanes) {
    State in{};
    for (int b = 0; b < 16; ++b) in.xmm[0][b] = static_cast<uint8_t>(b);
    for (int b = 0; b < 16; ++b) in.xmm[1][b] = static_cast<uint8_t>(15 - b);

    State out{};
    // PSHUFB xmm0, xmm1 -> 66 0F 38 00 C1
    ASSERT_TRUE(vitreo::oracle::execute(in, {0x66, 0x0F, 0x38, 0x00, 0xC1}, &out));
    ASSERT_EQ(out.status, 0u);

    // Result: lane i = xmm0[xmm1[i]] = xmm0[15-i] = 15-i
    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(out.xmm[0][i], static_cast<uint8_t>(15 - i)) << "lane " << i;
    }
}

TEST_F(OracleRunnerTest, PshufbMsbIndexZerosLane) {
    State in{};
    for (int b = 0; b < 16; ++b) in.xmm[0][b] = static_cast<uint8_t>(b + 1);
    in.xmm[1] = {0x80, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

    State out{};
    ASSERT_TRUE(vitreo::oracle::execute(in, {0x66, 0x0F, 0x38, 0x00, 0xC1}, &out));
    ASSERT_EQ(out.status, 0u);

    EXPECT_EQ(out.xmm[0][0], 0) << "lane 0 should be zero (idx msb set)";
    for (int i = 1; i < 16; ++i) {
        EXPECT_EQ(out.xmm[0][i], static_cast<uint8_t>(i + 1)) << "lane " << i;
    }
}

TEST_F(OracleRunnerTest, ReportsFaultForIllegalInstruction) {
    State in{};
    State out{};
    // UD2 -> 0F 0B (guaranteed invalid)
    ASSERT_TRUE(vitreo::oracle::execute(in, {0x0F, 0x0B}, &out));
    EXPECT_NE(out.status, 0u) << "UD2 should produce a fault status";
}

}  // namespace
