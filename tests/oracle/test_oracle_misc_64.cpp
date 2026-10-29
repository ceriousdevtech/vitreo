// SPDX-License-Identifier: LGPL-2.1-only
// x86-64 CBW/CWDE/CDQE/CWD/CDQ/CQO / FXSAVE / XSAVE ↔ Oracle (Intel SDM Vol. 2).
//
// CPUID and RDTSC/RDTSCP are not compared to Rosetta: leaf values and TSC
// are host-specific. See tools/fuzz/isa/fuzz_x64_misc.cpp.

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

class OracleMisc64 : public ::testing::Test {
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
        s.gpr[0] = 0x0000'1111'2222'3380ull;  // AL negative, AX positive, EAX +
        s.gpr[1] = 0x0000'4444'5555'6666ull;
        s.gpr[2] = 0x0000'7777'8888'9999ull;
        s.gpr[3] = 0x0000'AAAA'BBBB'CCCCull;
        s.gpr[7] = vitreo::oracle::kMemVa;
        return s;
    }

    static void run_triplet(const State& in, const std::vector<uint8_t>& code,
                            CompareOpts opts = {}) {
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

    static CompareOpts fxsave64_opts() {
        CompareOpts opts;
        opts.mem_skip_off = vitreo::oracle::contract::kFxsavePtrOff;
        opts.mem_skip_len = vitreo::oracle::contract::kFxsavePtrLen;
        opts.mem_skip2_off = vitreo::oracle::contract::kFxsaveStOff;
        opts.mem_skip2_len = vitreo::oracle::contract::kFxsaveStLen;
        opts.mem_skip3_off = vitreo::oracle::contract::kFxsave64RsvdOff;
        opts.mem_skip3_len = vitreo::oracle::contract::kFxsave64RsvdLen;
        return opts;
    }

    // XSAVE writes reserved FXSAVE byte 5; include it with FOP/FIP/FDP.
    static CompareOpts xsave_legacy_opts() {
        CompareOpts opts = fxsave64_opts();
        opts.mem_skip_off = 5;
        opts.mem_skip_len = vitreo::oracle::contract::kFxsavePtrLen + 1;
        return opts;
    }
};

// 66 98 CBW: AX := SignExtend(AL); EAX[31:16] and RAX[63:32] preserved.
TEST_F(OracleMisc64, Cbw) {
    State s = seed_state();
    s.gpr[0] = 0x0000ABCD00000080ull;
    run_triplet(s, {0x66, 0x98});
}

// 98 CWDE: EAX := SignExtend(AX); RAX[63:32] zero-extended.
TEST_F(OracleMisc64, Cwde) {
    State s = seed_state();
    s.gpr[0] = 0x0000ABCD00008000ull;
    run_triplet(s, {0x98});
}

// 48 98 CDQE: RAX := SignExtend(EAX).
TEST_F(OracleMisc64, Cdqe) {
    State s = seed_state();
    s.gpr[0] = 0x0000ABCD80000001ull;
    run_triplet(s, {0x48, 0x98});
}

// 66 99 CWD: DX := SignExtend(AX); EDX[31:16] and RDX[63:32] preserved.
TEST_F(OracleMisc64, Cwd) {
    State s = seed_state();
    s.gpr[0] = 0x0000ABCD00008000ull;
    s.gpr[2] = 0x0000123456789ABCull;
    run_triplet(s, {0x66, 0x99});
}

// 99 CDQ: EDX := SignExtend(EAX); RAX unchanged (no 32-bit EAX write).
TEST_F(OracleMisc64, Cdq) {
    State s = seed_state();
    s.gpr[0] = 0x0000ABCD80000001ull;
    s.gpr[2] = 0x1111111122222222ull;
    run_triplet(s, {0x99});
}

// 48 99 CQO: RDX := SignExtend(RAX).
TEST_F(OracleMisc64, Cqo) {
    State s = seed_state();
    s.gpr[0] = 0x8000'0000'0000'0001ull;
    s.gpr[2] = 0x0000'1111'2222'3333ull;
    run_triplet(s, {0x48, 0x99});
}

// Intel SDM Vol. 2 FXSAVE: 0F AE /0. Skip FOP/FIP/FDP and empty-ST leftovers.
TEST_F(OracleMisc64, FxsaveRdi) {
    State s = seed_state();
    s.xmm[0].fill(0xA5);
    s.xmm[1].fill(0x5A);
    run_triplet(s, {0x0F, 0xAE, 0x07}, fxsave64_opts());
}

// Intel SDM Vol. 2 FXSAVE64: REX.W 0F AE /0.
TEST_F(OracleMisc64, Fxsave64Rdi) {
    State s = seed_state();
    s.xmm[8].fill(0x3C);
    run_triplet(s, {0x48, 0x0F, 0xAE, 0x07}, fxsave64_opts());
}

// Intel SDM Vol. 2 FXRSTOR: 0F AE /1. Legal image; XMM0 restored.
TEST_F(OracleMisc64, FxrstorRdi) {
    State s = seed_state();
    s.mem.fill(0);
    s.mem[0] = 0x7F;
    s.mem[1] = 0x03;
    const uint32_t mx = 0x1F80u;
    const uint32_t mask = 0xFFFFu;
    std::memcpy(s.mem.data() + 24, &mx, 4);
    std::memcpy(s.mem.data() + 28, &mask, 4);
    for (int b = 0; b < 16; ++b) {
        s.mem[160 + b] = static_cast<uint8_t>(0x10 + b);
    }
    s.xmm[0].fill(0);
    run_triplet(s, {0x0F, 0xAE, 0x0F});
}

// Intel SDM Vol. 2 XSAVE: 0F AE /4. EAX:EDX = x87+SSE. Skip if host #UD.
TEST_F(OracleMisc64, XsaveRdi) {
    State s = seed_state();
    s.gpr[0] = 3;
    s.gpr[2] = 0;
    s.xmm[0].fill(0xA5);
    State oracle_out{};
    ASSERT_TRUE(vitreo::oracle::execute(s, {0x0F, 0xAE, 0x27}, &oracle_out));
    if (oracle_out.status != 0u) {
        GTEST_SKIP() << "host/Rosetta #UD or fault on XSAVE (CPUID.XSAVE)";
    }
    run_triplet(s, {0x0F, 0xAE, 0x27}, xsave_legacy_opts());
}

}  // namespace
