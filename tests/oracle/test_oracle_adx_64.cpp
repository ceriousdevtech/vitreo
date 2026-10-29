// SPDX-License-Identifier: LGPL-2.1-only
// x86-64 interpreter/JIT ↔ Oracle ADX tests (ADCX / ADOX).
//
// Same harness as test_oracle_integer_64.cpp. Fixture is OracleAdx64 so
// this TU does not ODR-collide with OracleInteger64 / OracleContract64.
//
// Intel SDM Vol. 2 ADCX / ADOX: dest := dest + src + {CF|OF}. ADCX writes
// only CF; ADOX writes only OF. 32-bit dest writes zero-extend in long mode.
//
// Rosetta 2 currently #UDs ADCX/ADOX (oracle status 2). In that case the
// interpreter is the semantic reference: planted SDM outcomes are checked
// directly, and JIT is compared to the interpreter.

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
using vitreo::oracle::contract::kArithFlagsMask;
using vitreo::oracle::contract::run_on_interpreter_64;
using vitreo::oracle::contract::run_on_jit_64;

class OracleAdx64 : public ::testing::Test {
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
                            uint64_t expect_rax, uint32_t expect_arith_flags,
                            CompareOpts opts = {}) {
        State oracle_out{};
        ASSERT_TRUE(vitreo::oracle::execute(in, code, &oracle_out))
            << "oracle execution failed";
        const bool oracle_ok = oracle_out.status == 0u;
        if (oracle_ok) {
            ASSERT_EQ(oracle_out.rip, vitreo::oracle::kCodeVa64)
                << "oracle did not place the snippet at the requested VA";
        } else {
            ASSERT_EQ(oracle_out.status, 2u)
                << "oracle snippet faulted with unexpected status";
        }

        State interp_out = run_on_interpreter_64(in, code);
        ASSERT_EQ(interp_out.status, 0u) << "interpreter snippet faulted";
        EXPECT_EQ(interp_out.gpr[0], expect_rax);
        EXPECT_EQ(interp_out.rflags & kArithFlagsMask, expect_arith_flags);

        if (oracle_ok) {
            std::string interp_diff = compare(interp_out, oracle_out, opts);
            EXPECT_TRUE(interp_diff.empty())
                << "interpreter ↔ oracle divergence:\n" << interp_diff;
        }

        State jit_out = run_on_jit_64(in, code);
        if (jit_out.status == 7u) {
            GTEST_SKIP() << "JIT has no native lowering for this snippet";
        }
        ASSERT_EQ(jit_out.status, 0u) << "JIT snippet faulted";
        EXPECT_EQ(jit_out.gpr[0], expect_rax);
        EXPECT_EQ(jit_out.rflags & kArithFlagsMask, expect_arith_flags);

        if (oracle_ok) {
            std::string jit_diff = compare(jit_out, oracle_out, opts);
            EXPECT_TRUE(jit_diff.empty())
                << "JIT ↔ oracle divergence:\n" << jit_diff;
        } else {
            std::string jit_diff = compare(jit_out, interp_out, opts);
            EXPECT_TRUE(jit_diff.empty())
                << "JIT ↔ interpreter divergence:\n" << jit_diff;
        }
    }
};

// ADCX RAX, RCX — 66 48 0F 38 F6 C1. CF=1 and dest+src+CF overflows 64 bits.
// All other arith flags start set and must be left unchanged.
TEST_F(OracleAdx64, AdcxRaxRcxCarryOut) {
    State s = seed_state();
    s.gpr[0] = 0xFFFF'FFFF'FFFF'FFFFull;
    s.gpr[1] = 1ull;
    s.rflags = 0x202 | kArithFlagsMask;
    run_triplet(s, {0x66, 0x48, 0x0F, 0x38, 0xF6, 0xC1}, 1ull, kArithFlagsMask);
}

// ADOX RAX, RCX — F3 48 0F 38 F6 C1. OF=1 and dest+src+OF overflows 64 bits.
TEST_F(OracleAdx64, AdoxRaxRcxOverflowCarryOut) {
    State s = seed_state();
    s.gpr[0] = 0xFFFF'FFFF'FFFF'FFFFull;
    s.gpr[1] = 1ull;
    s.rflags = 0x202 | kArithFlagsMask;
    run_triplet(s, {0xF3, 0x48, 0x0F, 0x38, 0xF6, 0xC1}, 1ull, kArithFlagsMask);
}

// ADCX EAX, ECX — 66 0F 38 F6 C1. High RAX bits start nonzero; dest must
// zero-extend to 64 bits (Intel SDM Vol. 2, 64-bit mode 32-bit dest write).
TEST_F(OracleAdx64, AdcxEaxEcxZeroExtends) {
    State s = seed_state();
    s.gpr[0] = 0xDEAD'BEEF'1234'5678ull;
    s.gpr[1] = 1ull;
    s.rflags = 0x202 | 0x001u;  // CF=1
    // 0x12345678 + 1 + CF = 0x1234567A; no 32-bit carry, so CF clears.
    run_triplet(s, {0x66, 0x0F, 0x38, 0xF6, 0xC1}, 0x0000'0000'1234'567Aull, 0u);
}

}  // namespace
