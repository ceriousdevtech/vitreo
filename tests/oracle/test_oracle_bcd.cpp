// SPDX-License-Identifier: LGPL-2.1-only
// 32-bit DAA/DAS/AAA/AAS/AAM/AAD ↔ interpreter (Intel SDM Vol. 2).
//
// Rosetta always runs long mode, so these opcodes #UD on the oracle. The
// tests still attempt execute(); a fault skips the oracle half and checks
// the interpreter against the SDM. PUSHA/POPA are catalog-only: the 32-bit
// oracle does not load ESP (rsp_live is 64-bit only).

#include "oracle/oracle_client.hpp"
#include "oracle/oracle_contract.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using vitreo::oracle::State;
using vitreo::oracle::contract::compare;
using vitreo::oracle::contract::CompareOpts;
using vitreo::oracle::contract::kArithFlagsMask;
using vitreo::oracle::contract::run_on_interpreter;
using vitreo::oracle::contract::run_on_jit;

constexpr uint64_t kFlBase = 0x202;
constexpr uint64_t kCF = 0x001;
constexpr uint64_t kPF = 0x004;
constexpr uint64_t kAF = 0x010;
constexpr uint64_t kZF = 0x040;
constexpr uint64_t kSF = 0x080;
constexpr uint64_t kOF = 0x800;

class OracleBcd32 : public ::testing::Test {
protected:
    void SetUp() override {
        if (!vitreo::oracle::available()) {
            GTEST_SKIP() << "oracle runner not available (requires the host x86 runner)";
        }
    }

    static State seed_state() {
        State s{};
        s.mode = vitreo::oracle::kMode32;
        s.rflags = kFlBase;
        s.mxcsr = 0x1F80;
        s.gpr[7] = vitreo::oracle::kMemVa;
        s.mem_va = vitreo::oracle::kMemVa;
        return s;
    }

    // Interpreter is the 32-bit semantic reference. Oracle compare is
    // optional: long-mode Rosetta #UDs these encodings.
    static void run_interp(const State& in, const std::vector<uint8_t>& code,
                           CompareOpts opts = {}) {
        State interp_out = run_on_interpreter(in, code);
        ASSERT_EQ(interp_out.status, 0u) << "interpreter snippet faulted";

        State oracle_out{};
        if (vitreo::oracle::execute(in, code, &oracle_out) &&
            oracle_out.status == 0u) {
            std::string diff = compare(interp_out, oracle_out, opts);
            EXPECT_TRUE(diff.empty()) << "interpreter ↔ oracle:\n" << diff;
        }

        State jit_out = run_on_jit(in, code);
        if (jit_out.status == 7u) return;
        ASSERT_EQ(jit_out.status, 0u) << "JIT snippet faulted";
        std::string jit_diff = compare(jit_out, interp_out, opts);
        EXPECT_TRUE(jit_diff.empty()) << "JIT ↔ interpreter:\n" << jit_diff;
    }
};

// Intel SDM Vol. 2 DAA example: AL=AEH after ADD, AF=0 CF=0 → AL=14H, CF=1 AF=1.
TEST_F(OracleBcd32, DaaAfterAddAe) {
    State s = seed_state();
    s.gpr[0] = 0xAE;
    s.rflags = kFlBase;
    CompareOpts opts;
    opts.flags_mask = kArithFlagsMask & ~static_cast<uint32_t>(kOF);
    run_interp(s, {0x27}, opts);
    State out = run_on_interpreter(s, {0x27});
    ASSERT_EQ(out.status, 0u);
    EXPECT_EQ(out.gpr[0] & 0xFFu, 0x14u);
    EXPECT_NE(out.rflags & kCF, 0u);
    EXPECT_NE(out.rflags & kAF, 0u);
}

// AL=0x0A, AF=0 CF=0 → AL=0x10, AF=1 CF=0 (low-nibble adjust only).
TEST_F(OracleBcd32, DaaLowNibble) {
    State s = seed_state();
    s.gpr[0] = 0x0A;
    State out = run_on_interpreter(s, {0x27});
    ASSERT_EQ(out.status, 0u);
    EXPECT_EQ(out.gpr[0] & 0xFFu, 0x10u);
    EXPECT_EQ(out.rflags & kCF, 0u);
    EXPECT_NE(out.rflags & kAF, 0u);
}

// DAS: AL=0x05 AF=1 CF=0 → low adjust sets a temporary CF, second IF else
// clears it (Intel SDM Vol. 2 DAS). AL=0xFF.
TEST_F(OracleBcd32, DasClearsCfAfterLowAdjustOnly) {
    State s = seed_state();
    s.gpr[0] = 0x05;
    s.rflags = kFlBase | kAF;
    State out = run_on_interpreter(s, {0x2F});
    ASSERT_EQ(out.status, 0u);
    EXPECT_EQ(out.gpr[0] & 0xFFu, 0xFFu);
    EXPECT_EQ(out.rflags & kCF, 0u);
    EXPECT_NE(out.rflags & kAF, 0u);
    EXPECT_NE(out.rflags & kSF, 0u);
}

TEST_F(OracleBcd32, AaaSetsCfAf) {
    State s = seed_state();
    s.gpr[0] = 0x000A;  // AL=0x0A
    CompareOpts opts;
    opts.flags_mask = static_cast<uint32_t>(kCF | kAF);
    run_interp(s, {0x37}, opts);
    State out = run_on_interpreter(s, {0x37});
    ASSERT_EQ(out.status, 0u);
    EXPECT_EQ(out.gpr[0] & 0xFFu, 0x00u);
    EXPECT_EQ((out.gpr[0] >> 8) & 0xFFu, 0x01u);
    EXPECT_NE(out.rflags & kCF, 0u);
    EXPECT_NE(out.rflags & kAF, 0u);
}

TEST_F(OracleBcd32, AamBase10) {
    State s = seed_state();
    s.gpr[0] = 0x17;  // 23 → AH=2 AL=3
    CompareOpts opts;
    opts.flags_mask = static_cast<uint32_t>(kPF | kZF | kSF);
    run_interp(s, {0xD4, 0x0A}, opts);
    State out = run_on_interpreter(s, {0xD4, 0x0A});
    ASSERT_EQ(out.status, 0u);
    EXPECT_EQ(out.gpr[0] & 0xFFu, 0x03u);
    EXPECT_EQ((out.gpr[0] >> 8) & 0xFFu, 0x02u);
}

TEST_F(OracleBcd32, AadBase10) {
    State s = seed_state();
    s.gpr[0] = 0x0203;  // AH=2 AL=3 → AL=23
    CompareOpts opts;
    opts.flags_mask = static_cast<uint32_t>(kPF | kZF | kSF);
    run_interp(s, {0xD5, 0x0A}, opts);
    State out = run_on_interpreter(s, {0xD5, 0x0A});
    ASSERT_EQ(out.status, 0u);
    EXPECT_EQ(out.gpr[0] & 0xFFFFu, 0x0017u);
}

}  // namespace
