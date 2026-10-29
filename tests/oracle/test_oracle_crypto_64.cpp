// SPDX-License-Identifier: LGPL-2.1-only
// x86-64 interpreter/JIT ↔ Oracle AES-NI / SHA-NI / MOVBE tests.
//
// Same harness as test_oracle_avx_64.cpp. Fixture is OracleCrypto64 so
// this TU does not ODR-collide with OracleAvx64 / OracleInteger64.
//
// Intel SDM Vol. 2 AESENC: DEST := MixColumns(SubBytes(ShiftRows(DEST))) XOR SRC.
// Intel SDM Vol. 2 SHA1NEXTE / SHA1RNDS4 / SHA256*: SHA-NI; #UD without
// CPUID.07H:EBX.SHA. Intel SDM Vol. 2 MOVBE: byte-swap load/store; flags
// unaffected.

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

class OracleCrypto64 : public ::testing::Test {
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

    // SHA-NI #UDs on hosts without CPUID.07H:EBX.SHA (typical Rosetta).
    static void run_triplet_skip_ud(const State& in,
                                    const std::vector<uint8_t>& code) {
        State oracle_out{};
        ASSERT_TRUE(vitreo::oracle::execute(in, code, &oracle_out))
            << "oracle execution failed";
        if (oracle_out.status != 0u) {
            GTEST_SKIP() << "oracle snippet faulted (missing SHA-NI CPUID)";
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

// AESENC xmm0, xmm1 — 66 0F 38 DC C1. One AES encryption round; flags unchanged.
TEST_F(OracleCrypto64, AesencXmm0Xmm1) {
    State s = seed_state();
    s.rflags = 0x202 | vitreo::oracle::contract::kArithFlagsMask;
    run_triplet(s, {0x66, 0x0F, 0x38, 0xDC, 0xC1});
}

// MOVBE RAX, [RDI] — 48 0F 38 F0 07. Byte-swap load of a known 8-byte pattern.
TEST_F(OracleCrypto64, MovbeRaxMem) {
    State s = seed_state();
    s.rflags = 0x202 | vitreo::oracle::contract::kArithFlagsMask;
    static const uint8_t kPattern[8] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    std::memcpy(s.mem.data(), kPattern, sizeof(kPattern));
    run_triplet(s, {0x48, 0x0F, 0x38, 0xF0, 0x07});
}

// Intel SDM Vol. 2 SHA1NEXTE: NP 0F 38 C8 /r. DEST[127:96] := SRC[127:96] +
// (DEST[127:96] ROL 30); low 96 bits copied from SRC. Flags unaffected.
TEST_F(OracleCrypto64, Sha1nexteXmm0Xmm1) {
    State s = seed_state();
    s.rflags = 0x202 | vitreo::oracle::contract::kArithFlagsMask;
    run_triplet_skip_ud(s, {0x0F, 0x38, 0xC8, 0xC1});
}

TEST_F(OracleCrypto64, Sha1msg1Xmm0Xmm1) {
    State s = seed_state();
    s.rflags = 0x202 | vitreo::oracle::contract::kArithFlagsMask;
    run_triplet_skip_ud(s, {0x0F, 0x38, 0xC9, 0xC1});
}

TEST_F(OracleCrypto64, Sha1msg2Xmm0Xmm1) {
    State s = seed_state();
    s.rflags = 0x202 | vitreo::oracle::contract::kArithFlagsMask;
    run_triplet_skip_ud(s, {0x0F, 0x38, 0xCA, 0xC1});
}

// Intel SDM Vol. 2 SHA1RNDS4: NP 0F 3A CC /r ib. imm8 selects f() and K.
TEST_F(OracleCrypto64, Sha1rnds4Imm0) {
    State s = seed_state();
    s.rflags = 0x202 | vitreo::oracle::contract::kArithFlagsMask;
    run_triplet_skip_ud(s, {0x0F, 0x3A, 0xCC, 0xC1, 0x00});
}

TEST_F(OracleCrypto64, Sha1rnds4Imm3) {
    State s = seed_state();
    s.rflags = 0x202 | vitreo::oracle::contract::kArithFlagsMask;
    run_triplet_skip_ud(s, {0x0F, 0x3A, 0xCC, 0xC1, 0x03});
}

// Intel SDM Vol. 2 SHA256RNDS2: NP 0F 38 CB /r. Implicit XMM0 is W+K.
TEST_F(OracleCrypto64, Sha256rnds2Xmm0Xmm1) {
    State s = seed_state();
    s.rflags = 0x202 | vitreo::oracle::contract::kArithFlagsMask;
    run_triplet_skip_ud(s, {0x0F, 0x38, 0xCB, 0xC1});
}

TEST_F(OracleCrypto64, Sha256msg1Xmm0Xmm1) {
    State s = seed_state();
    s.rflags = 0x202 | vitreo::oracle::contract::kArithFlagsMask;
    run_triplet_skip_ud(s, {0x0F, 0x38, 0xCC, 0xC1});
}

TEST_F(OracleCrypto64, Sha256msg2Xmm0Xmm1) {
    State s = seed_state();
    s.rflags = 0x202 | vitreo::oracle::contract::kArithFlagsMask;
    run_triplet_skip_ud(s, {0x0F, 0x38, 0xCD, 0xC1});
}

}  // namespace
