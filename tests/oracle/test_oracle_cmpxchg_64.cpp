// SPDX-License-Identifier: LGPL-2.1-only
// Planted x64 CMPXCHG8B/16B interpreter/JIT ↔ x86 oracle tests.
//
// Intel SDM Vol. 2 CMPXCHG8B/CMPXCHG16B: compare EDX:EAX (RDX:RAX) with
// m64 (aligned m128). Equal → dest := ECX:EBX (RCX:RBX), ZF=1. Else
// EDX:EAX (RDX:RAX) := dest, ZF=0. Other arith flags are unchanged.

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

class OracleCmpxchg64 : public ::testing::Test {
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
};

// CMPXCHG16B [RDI] — 48 0F C7 0F. RDI = kMemVa (16-byte aligned).
TEST_F(OracleCmpxchg64, Cmpxchg16bMatch) {
    State s = seed_state();
    const uint64_t lo = 0x1111111111111111ull;
    const uint64_t hi = 0x2222222222222222ull;
    std::memcpy(s.mem.data(), &lo, 8);
    std::memcpy(s.mem.data() + 8, &hi, 8);
    s.gpr[0] = lo;                     // RAX
    s.gpr[2] = hi;                     // RDX
    s.gpr[3] = 0x3333333333333333ull;  // RBX
    s.gpr[1] = 0x4444444444444444ull;  // RCX
    s.rflags = 0x202 | 0x8D5;          // all arith flags set; only ZF must change
    run_triplet(s, {0x48, 0x0F, 0xC7, 0x0F});
}

TEST_F(OracleCmpxchg64, Cmpxchg16bMismatch) {
    State s = seed_state();
    const uint64_t lo = 0xAAAAAAAAAAAAAAAAULL;
    const uint64_t hi = 0xBBBBBBBBBBBBBBBBULL;
    std::memcpy(s.mem.data(), &lo, 8);
    std::memcpy(s.mem.data() + 8, &hi, 8);
    s.gpr[0] = 1;  // RAX ≠ mem lo
    s.gpr[2] = 2;  // RDX ≠ mem hi
    s.gpr[3] = 3;  // RBX
    s.gpr[1] = 4;  // RCX
    s.rflags = 0x202 | 0x8D5;
    run_triplet(s, {0x48, 0x0F, 0xC7, 0x0F});
}

// CMPXCHG8B [RDI] — 0F C7 0F. EDX:EAX match m64 → store ECX:EBX, ZF=1.
TEST_F(OracleCmpxchg64, Cmpxchg8bMatch) {
    State s = seed_state();
    const uint32_t lo = 0xAAAAAAAAu;
    const uint32_t hi = 0xBBBBBBBBu;
    std::memcpy(s.mem.data(), &lo, 4);
    std::memcpy(s.mem.data() + 4, &hi, 4);
    s.gpr[0] = lo;          // EAX (upper half of RAX left as seed; unused)
    s.gpr[2] = hi;          // EDX
    s.gpr[3] = 0x11111111u; // EBX
    s.gpr[1] = 0x22222222u; // ECX
    s.rflags = 0x202 | 0x8D5;
    run_triplet(s, {0x0F, 0xC7, 0x0F});
}

}  // namespace
