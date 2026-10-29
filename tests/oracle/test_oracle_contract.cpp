// SPDX-License-Identifier: LGPL-2.1-only
// Interpreter ↔ Oracle contract tests. Each test case picks a snippet,
// runs the same input state through Vitreo's interpreter and the host x86
// oracle, and asserts the post-state matches.

#include "oracle/oracle_client.hpp"
#include "oracle/oracle_contract.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

namespace {

using vitreo::oracle::State;
using vitreo::oracle::contract::run_on_interpreter;
using vitreo::oracle::contract::run_on_jit;
using vitreo::oracle::contract::compare;
using vitreo::oracle::contract::CompareOpts;

class InterpreterOracleContract : public ::testing::Test {
protected:
    void SetUp() override {
        if (!vitreo::oracle::available()) {
            GTEST_SKIP() << "oracle runner not available (requires the host x86 runner)";
        }
    }

    static State zero_state() {
        State s{};
        s.rflags = 0x202;
        s.mxcsr = 0x1F80;
        return s;
    }

    static void put_u32(State& state, size_t offset, uint32_t value) {
        std::memcpy(state.mem.data() + offset, &value, sizeof(value));
    }

    static void run_pair(const State& in, const std::vector<uint8_t>& code,
                         CompareOpts opts = {}) {
        State oracle_out{};
        ASSERT_TRUE(vitreo::oracle::execute(in, code, &oracle_out))
            << "oracle execution failed";
        State interp_out = run_on_interpreter(in, code);

        std::string diff = compare(interp_out, oracle_out, opts);
        EXPECT_TRUE(diff.empty())
            << "interpreter ↔ oracle divergence:\n" << diff;
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

TEST_F(InterpreterOracleContract, AddEaxEcx) {
    State in = zero_state();
    in.gpr[0] = 0xAAAAAAAAu;
    in.gpr[1] = 0xCCCCCCCCu;
    run_pair(in, {0x01, 0xC8});  // ADD EAX, ECX
}

TEST_F(InterpreterOracleContract, AddSetsZF) {
    State in = zero_state();
    in.gpr[0] = 0x00000001u;
    in.gpr[1] = 0xFFFFFFFFu;
    run_pair(in, {0x01, 0xC8});  // ADD EAX, ECX -> 0, CF=1, ZF=1
}

TEST_F(InterpreterOracleContract, SubEaxEbx) {
    State in = zero_state();
    in.gpr[0] = 0x12345678u;
    in.gpr[3] = 0x00000078u;
    run_pair(in, {0x29, 0xD8});  // SUB EAX, EBX
}

TEST_F(InterpreterOracleContract, XorEaxEax) {
    State in = zero_state();
    in.gpr[0] = 0xDEADBEEFu;
    run_pair(in, {0x31, 0xC0});  // XOR EAX, EAX -> ZF=1, CF=0, OF=0
}

TEST_F(InterpreterOracleContract, IncEax) {
    State in = zero_state();
    in.gpr[0] = 0x7FFFFFFFu;
    // NOTE: opcode 0x40 is INC EAX in 32-bit mode but a REX prefix in 64-bit
    // mode (which the x86 oracle runs in). Use the ModR/M form FF /0
    // which has the same encoding and semantics in both modes.
    run_pair(in, {0xFF, 0xC0});  // INC EAX -> OF=1, CF unchanged
}

TEST_F(InterpreterOracleContract, PxorSelfClearsXmm) {
    State in = zero_state();
    in.xmm[0].fill(0xAA);
    in.xmm[1].fill(0x55);
    run_pair(in, {0x66, 0x0F, 0xEF, 0xC0});  // PXOR xmm0, xmm0
}

TEST_F(InterpreterOracleContract, PaddwXmm0Xmm1) {
    State in = zero_state();
    for (int i = 0; i < 8; ++i) {
        in.xmm[0][2*i + 0] = static_cast<uint8_t>(0x10 + i);
        in.xmm[0][2*i + 1] = 0;
        in.xmm[1][2*i + 0] = static_cast<uint8_t>(0x20 + i);
        in.xmm[1][2*i + 1] = 0;
    }
    run_pair(in, {0x66, 0x0F, 0xFD, 0xC1});  // PADDW xmm0, xmm1
}

TEST_F(InterpreterOracleContract, PshufbReverseLanes) {
    State in = zero_state();
    for (int b = 0; b < 16; ++b) {
        in.xmm[0][b] = static_cast<uint8_t>(b);
        in.xmm[1][b] = static_cast<uint8_t>(15 - b);
    }
    run_pair(in, {0x66, 0x0F, 0x38, 0x00, 0xC1});  // PSHUFB xmm0, xmm1
}

TEST_F(InterpreterOracleContract, PshufbMsbZeroesLane) {
    State in = zero_state();
    for (int b = 0; b < 16; ++b) in.xmm[0][b] = static_cast<uint8_t>(b + 1);
    in.xmm[1] = {0x80, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    run_pair(in, {0x66, 0x0F, 0x38, 0x00, 0xC1});
}

TEST_F(InterpreterOracleContract, PaddbRandom) {
    std::mt19937 rng(0xC0FFEEu);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    for (int trial = 0; trial < 8; ++trial) {
        State in = zero_state();
        for (int b = 0; b < 16; ++b) {
            in.xmm[0][b] = static_cast<uint8_t>(byte_dist(rng));
            in.xmm[1][b] = static_cast<uint8_t>(byte_dist(rng));
        }
        SCOPED_TRACE("trial " + std::to_string(trial));
        run_pair(in, {0x66, 0x0F, 0xFC, 0xC1});  // PADDB xmm0, xmm1
    }
}

TEST_F(InterpreterOracleContract, BattleNetOpenSslMulAddCarryWindow) {
    // Exact Agent.9700 bytes from 0x603fb4 through 0x603fde. This is the first
    // two-limb window of the bundled OpenSSL bignum loop active during local
    // certificate generation:
    //
    //   mov [ebp+10h],esi; mov eax,[ebp+14h]; mul [esi]
    //   add eax,[edi]; mov ecx,edx; adc ecx,0
    //   add eax,[ebp-4]; mov [edi],eax; mov eax,[ebp+14h]
    //   adc ecx,0; mul [esi+4]; add eax,ecx; mov esi,edx
    //   mov ecx,[ebp+10h]; adc esi,0; add [edi+4],eax
    static const std::vector<uint8_t> code{
        0x89, 0x75, 0x10,
        0x8B, 0x45, 0x14,
        0xF7, 0x26,
        0x03, 0x07,
        0x8B, 0xCA,
        0x83, 0xD1, 0x00,
        0x03, 0x45, 0xFC,
        0x89, 0x07,
        0x8B, 0x45, 0x14,
        0x83, 0xD1, 0x00,
        0xF7, 0x66, 0x04,
        0x03, 0xC1,
        0x8B, 0xF2,
        0x8B, 0x4D, 0x10,
        0x83, 0xD6, 0x00,
        0x01, 0x47, 0x04,
    };

    // carry word, multiplier, source limbs 0/1, destination limbs 0/1.
    static const std::array<std::array<uint32_t, 6>, 8> cases{{
        {{0x00000000u, 0x00000000u, 0x00000000u,
          0x00000000u, 0x00000000u, 0x00000000u}},
        {{0x00000000u, 0xFFFFFFFFu, 0xFFFFFFFFu,
          0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu}},
        {{0x00000001u, 0xFFFFFFFFu, 0x00000002u,
          0x00000003u, 0xFFFFFFFFu, 0xFFFFFFFFu}},
        {{0xFFFFFFFFu, 0x80000000u, 0xFFFFFFFFu,
          0x7FFFFFFFu, 0x80000000u, 0x00000001u}},
        {{0x89ABCDEFu, 0x10203041u, 0x55667788u,
          0x99AABBCCu, 0xDDEEFF00u, 0x12345678u}},
        {{0x7FFFFFFFu, 0x7FFFFFFFu, 0x80000000u,
          0x80000001u, 0x7FFFFFFFu, 0x80000000u}},
        {{0xFFFFFFFEu, 0x00000002u, 0xFFFFFFFFu,
          0x00000001u, 0x00000001u, 0xFFFFFFFEu}},
        {{0x13579BDFu, 0xFEDCBA98u, 0x76543210u,
          0x89ABCDEFu, 0x0BADF00Du, 0xC001D00Du}},
    }};

    for (size_t trial = 0; trial < cases.size(); ++trial) {
        SCOPED_TRACE("trial " + std::to_string(trial));
        State in = zero_state();
        in.gpr[5] = vitreo::oracle::kMemVa + 4;   // EBP
        in.gpr[6] = vitreo::oracle::kMemVa + 32;  // ESI
        in.gpr[7] = vitreo::oracle::kMemVa + 48;  // EDI
        put_u32(in, 0, cases[trial][0]);
        put_u32(in, 24, cases[trial][1]);
        put_u32(in, 32, cases[trial][2]);
        put_u32(in, 36, cases[trial][3]);
        put_u32(in, 48, cases[trial][4]);
        put_u32(in, 52, cases[trial][5]);
        run_triplet(in, code);
    }
}

TEST_F(InterpreterOracleContract, BattleNetOpenSslSquareReductionWindow) {
    // Exact Agent.9700 bytes from 0x604c81 through 0x604ca7. This window from
    // the unrolled OpenSSL square/reduction routine exercises MUL carry,
    // ADD/ADC propagation, CMP/SBB borrow conversion, and NEG:
    //
    //   mov eax,[ebx+4]; mul eax; mov ecx,[ebp+0ch]; mov ebx,eax
    //   add ebx,esi; adc edx,0; cmp esi,edi; sbb eax,eax; neg eax
    //   add eax,edx; cmp eax,edx; mov [ebp-0ch],eax
    //   sbb eax,eax; neg eax; mov [ebp-8],eax; mov eax,[ecx+8]
    static const std::vector<uint8_t> code{
        0x8B, 0x43, 0x04,
        0xF7, 0xE0,
        0x8B, 0x4D, 0x0C,
        0x8B, 0xD8,
        0x03, 0xDE,
        0x83, 0xD2, 0x00,
        0x3B, 0xF7,
        0x1B, 0xC0,
        0xF7, 0xD8,
        0x03, 0xC2,
        0x3B, 0xC2,
        0x89, 0x45, 0xF4,
        0x1B, 0xC0,
        0xF7, 0xD8,
        0x89, 0x45, 0xF8,
        0x8B, 0x41, 0x08,
    };

    // Source limbs 1/2 and incoming ESI/EDI accumulators.
    static const std::array<std::array<uint32_t, 4>, 8> cases{{
        {{0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {{0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu}},
        {{0x80000000u, 0x7FFFFFFFu, 0x00000000u, 0xFFFFFFFFu}},
        {{0x00000002u, 0x00000003u, 0xFFFFFFFFu, 0x00000000u}},
        {{0x10203041u, 0x55667788u, 0x99AABBCCu, 0xDDEEFF00u}},
        {{0x7FFFFFFFu, 0x80000000u, 0x80000000u, 0x7FFFFFFFu}},
        {{0xFFFFFFFEu, 0x00000001u, 0x00000001u, 0xFFFFFFFEu}},
        {{0xFEDCBA98u, 0x76543210u, 0x89ABCDEFu, 0x0BADF00Du}},
    }};

    for (size_t trial = 0; trial < cases.size(); ++trial) {
        SCOPED_TRACE("trial " + std::to_string(trial));
        State in = zero_state();
        in.gpr[3] = vitreo::oracle::kMemVa + 32;  // EBX
        in.gpr[5] = vitreo::oracle::kMemVa + 16;  // EBP
        in.gpr[6] = cases[trial][2];               // ESI
        in.gpr[7] = cases[trial][3];               // EDI
        put_u32(in, 28, vitreo::oracle::kMemVa + 32);
        put_u32(in, 36, cases[trial][0]);
        put_u32(in, 40, cases[trial][1]);
        run_triplet(in, code);
    }
}

// ============================================================================
// Campaign ISA: SSE4.1 / SSE4.2 / CF / MXCSR vs the host x86 runner (32-bit encodings)
// ============================================================================

static State sse_state() {
    State s{};
    s.mxcsr = 0x1F80;
    s.gpr[7] = vitreo::oracle::kMemVa;  // EDI → scratch page
    s.rflags = 0x202 | 0x8D5;           // all arith flags set; ops rewrite them
    return s;
}

static void put_f32_lane(State& s, int xmm, int lane, float v) {
    std::memcpy(s.xmm[xmm].data() + lane * 4, &v, 4);
}

static void put_f64_lane(State& s, int xmm, int lane, double v) {
    std::memcpy(s.xmm[xmm].data() + lane * 8, &v, 8);
}

static void fill_xmm(State& s, int r, uint8_t v) { s.xmm[r].fill(v); }

TEST_F(InterpreterOracleContract, ClcClearsCfOnly) {
    State in = sse_state();
    run_triplet(in, {0xF8});  // CLC
}

TEST_F(InterpreterOracleContract, StcSetsCfOnly) {
    State in = sse_state();
    in.rflags = 0x202;  // CF clear
    run_triplet(in, {0xF9});  // STC
}

TEST_F(InterpreterOracleContract, CmcTogglesCfOnly) {
    State in = sse_state();
    run_triplet(in, {0xF5});  // CMC
}

TEST_F(InterpreterOracleContract, PopcntEaxEcx) {
    State in = sse_state();
    in.gpr[0] = 0xFFFFFFFFu;
    in.gpr[1] = 0xF0F0F0F0u;
    run_triplet(in, {0xF3, 0x0F, 0xB8, 0xC1});  // POPCNT EAX, ECX
}

TEST_F(InterpreterOracleContract, PhminposuwXmm0Xmm1) {
    State in = sse_state();
    const uint16_t words[8] = {9, 8, 7, 1, 6, 5, 4, 3};  // min=1 at index 3
    std::memcpy(in.xmm[1].data(), words, 16);
    in.xmm[0].fill(0xFF);
    run_triplet(in, {0x66, 0x0F, 0x38, 0x41, 0xC1});  // PHMINPOSUW XMM0, XMM1
}

TEST_F(InterpreterOracleContract, PcmpistriEqualEach) {
    State in = sse_state();
    in.xmm[0].fill(0);
    in.xmm[1].fill(0);
    std::memcpy(in.xmm[0].data(), "Hello", 6);
    std::memcpy(in.xmm[1].data(), "Hello", 6);
    run_triplet(in, {0x66, 0x0F, 0x3A, 0x63, 0xC1, 0x08});  // PCMPISTRI XMM0,XMM1,0x08
}

TEST_F(InterpreterOracleContract, PcmpistrmEqualEach) {
    State in = sse_state();
    in.xmm[0].fill(0);
    in.xmm[1].fill(0);
    in.xmm[2].fill(0);
    std::memcpy(in.xmm[1].data(), "Hello", 6);
    std::memcpy(in.xmm[2].data(), "Hello", 6);
    run_triplet(in, {0x66, 0x0F, 0x3A, 0x62, 0xCA, 0x08});  // PCMPISTRM XMM1,XMM2,0x08
}

TEST_F(InterpreterOracleContract, PcmpestriExplicitLengthMsb) {
    State in = sse_state();
    in.xmm[0].fill('X');
    in.xmm[1].fill('Y');
    std::memcpy(in.xmm[0].data(), "AAA", 3);
    std::memcpy(in.xmm[1].data(), "AAA", 3);
    in.gpr[0] = 3;  // EAX length
    in.gpr[2] = 3;  // EDX length
    run_triplet(in, {0x66, 0x0F, 0x3A, 0x61, 0xC1, 0x48});  // PCMPESTRI XMM0,XMM1,0x48
}

TEST_F(InterpreterOracleContract, DppsAllLanes) {
    State in = sse_state();
    put_f32_lane(in, 0, 0, 1.0f);
    put_f32_lane(in, 0, 1, 2.0f);
    put_f32_lane(in, 0, 2, 3.0f);
    put_f32_lane(in, 0, 3, 4.0f);
    put_f32_lane(in, 1, 0, 5.0f);
    put_f32_lane(in, 1, 1, 6.0f);
    put_f32_lane(in, 1, 2, 7.0f);
    put_f32_lane(in, 1, 3, 8.0f);
    run_triplet(in, {0x66, 0x0F, 0x3A, 0x40, 0xC1, 0xFF});  // DPPS XMM0,XMM1,0xFF
}

TEST_F(InterpreterOracleContract, DppdAllLanes) {
    State in = sse_state();
    put_f64_lane(in, 0, 0, 1.0);
    put_f64_lane(in, 0, 1, 2.0);
    put_f64_lane(in, 1, 0, 3.0);
    put_f64_lane(in, 1, 1, 4.0);
    run_triplet(in, {0x66, 0x0F, 0x3A, 0x41, 0xC1, 0x33});  // DPPD XMM0,XMM1,0x33
}

TEST_F(InterpreterOracleContract, MpsadbwImm0) {
    State in = sse_state();
    for (int b = 0; b < 16; ++b) {
        in.xmm[0][b] = static_cast<uint8_t>(b + 1);
        in.xmm[1][b] = static_cast<uint8_t>(16 - b);
    }
    run_triplet(in, {0x66, 0x0F, 0x3A, 0x42, 0xC1, 0x00});  // MPSADBW XMM0,XMM1,0
}

TEST_F(InterpreterOracleContract, PblendvbXmm1Xmm2) {
    State in = sse_state();
    in.xmm[0] = {0x80, 0x00, 0x80, 0x00, 0xFF, 0x7F, 0x00, 0x80,
                 0x80, 0x80, 0x00, 0x00, 0x7F, 0x80, 0xFF, 0x00};
    in.xmm[1].fill(0x11);
    in.xmm[2].fill(0xAA);
    run_triplet(in, {0x66, 0x0F, 0x38, 0x10, 0xCA});  // PBLENDVB XMM1, XMM2
}

TEST_F(InterpreterOracleContract, BlendvpsXmm1Xmm2) {
    State in = sse_state();
    in.xmm[0] = {0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00,
                 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00};
    in.xmm[1].fill(0x11);
    in.xmm[2].fill(0xAA);
    run_triplet(in, {0x66, 0x0F, 0x38, 0x14, 0xCA});  // BLENDVPS XMM1, XMM2
}

TEST_F(InterpreterOracleContract, BlendvpdXmm1Xmm2) {
    State in = sse_state();
    in.xmm[0] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    in.xmm[1].fill(0x11);
    in.xmm[2].fill(0xAA);
    run_triplet(in, {0x66, 0x0F, 0x38, 0x15, 0xCA});  // BLENDVPD XMM1, XMM2
}

TEST_F(InterpreterOracleContract, MovntdqaFromEdi) {
    State in = sse_state();
    for (int b = 0; b < 16; ++b) {
        in.mem[b] = static_cast<uint8_t>(0xA0 + b);
    }
    in.xmm[0].fill(0);
    run_triplet(in, {0x66, 0x0F, 0x38, 0x2A, 0x07});  // MOVNTDQA XMM0, [EDI]
}

TEST_F(InterpreterOracleContract, MovntiEdiEax) {
    State in = sse_state();
    in.gpr[0] = 0x89ABCDEFu;
    run_triplet(in, {0x0F, 0xC3, 0x07});  // MOVNTI [EDI], EAX
}

TEST_F(InterpreterOracleContract, LdmxcsrThenStmxcsr) {
    State in = sse_state();
    const uint32_t mx = 0x7F80u;  // FTZ + default exceptions masked
    std::memcpy(in.mem.data(), &mx, 4);
    // LDMXCSR [EDI]; STMXCSR [EDI+4]
    run_triplet(in, {0x0F, 0xAE, 0x17, 0x0F, 0xAE, 0x5F, 0x04});
}

TEST_F(InterpreterOracleContract, LahfSahfRoundTrip) {
    State in = sse_state();
    run_triplet(in, {0x9F, 0x9E});  // LAHF; SAHF
}

TEST_F(InterpreterOracleContract, FxsaveEdi) {
    State in = sse_state();
    in.xmm[0].fill(0xA5);
    in.xmm[1].fill(0x5A);
    in.mxcsr = 0x1F80;
    CompareOpts opts;
    opts.mem_skip_off = vitreo::oracle::contract::kFxsavePtrOff;
    opts.mem_skip_len = vitreo::oracle::contract::kFxsavePtrLen;
    opts.mem_skip2_off = vitreo::oracle::contract::kFxsaveStOff;
    opts.mem_skip2_len = vitreo::oracle::contract::kFxsaveStLen;
    opts.mem_skip3_off = vitreo::oracle::contract::kFxsaveLegacyRsvdOff;
    opts.mem_skip3_len = vitreo::oracle::contract::kFxsaveLegacyRsvdLen;
    run_triplet(in, {0x0F, 0xAE, 0x07}, opts);  // FXSAVE [EDI]
}

TEST_F(InterpreterOracleContract, FxrstorEdi) {
    State in = sse_state();
    in.mem.fill(0);
    in.mem[0] = 0x7F;
    in.mem[1] = 0x03;  // FCW = 0x037F
    const uint32_t mx = 0x1F80u;
    const uint32_t mask = 0xFFFFu;
    std::memcpy(in.mem.data() + 24, &mx, 4);
    std::memcpy(in.mem.data() + 28, &mask, 4);
    for (int b = 0; b < 16; ++b) {
        in.mem[160 + b] = static_cast<uint8_t>(0x10 + b);
    }
    in.xmm[0].fill(0);
    run_triplet(in, {0x0F, 0xAE, 0x0F});  // FXRSTOR [EDI]
}

TEST_F(InterpreterOracleContract, RdtscLeavesOtherState) {
    State in = sse_state();
    CompareOpts opts;
    opts.gpr_ignore_mask = vitreo::oracle::contract::kGprRax |
                           vitreo::oracle::contract::kGprRdx;
    run_triplet(in, {0x0F, 0x31}, opts);  // RDTSC
}

TEST_F(InterpreterOracleContract, RdtscpLeavesOtherState) {
    State in = sse_state();
    CompareOpts opts;
    opts.gpr_ignore_mask = vitreo::oracle::contract::kGprRax |
                           vitreo::oracle::contract::kGprRcx |
                           vitreo::oracle::contract::kGprRdx;
    run_triplet(in, {0x0F, 0x01, 0xF9}, opts);  // RDTSCP
}

TEST_F(InterpreterOracleContract, CmovzTaken) {
    State in = sse_state();
    in.gpr[0] = 0x11111111u;
    in.gpr[1] = 0x22222222u;
    in.rflags = 0x202 | 0x40;  // ZF
    run_triplet(in, {0x0F, 0x44, 0xC1});  // CMOVZ EAX, ECX
}

TEST_F(InterpreterOracleContract, CmovzNotTaken) {
    State in = sse_state();
    in.gpr[0] = 0x11111111u;
    in.gpr[1] = 0x22222222u;
    in.rflags = 0x202;
    run_triplet(in, {0x0F, 0x44, 0xC1});  // CMOVZ EAX, ECX
}

TEST_F(InterpreterOracleContract, Cmpxchg8bEqual) {
    State in = sse_state();
    put_u32(in, 0, 0xAAAAAAAAu);
    put_u32(in, 4, 0xBBBBBBBBu);
    in.gpr[0] = 0xAAAAAAAAu;  // EAX
    in.gpr[2] = 0xBBBBBBBBu;  // EDX
    in.gpr[3] = 0x11111111u;  // EBX
    in.gpr[1] = 0x22222222u;  // ECX
    run_triplet(in, {0x0F, 0xC7, 0x0F});  // CMPXCHG8B [EDI]
}

TEST_F(InterpreterOracleContract, Cmpxchg8bNotEqual) {
    State in = sse_state();
    put_u32(in, 0, 0x11111111u);
    put_u32(in, 4, 0x22222222u);
    in.gpr[0] = 0xFFFFFFFFu;
    in.gpr[2] = 0xFFFFFFFFu;
    in.gpr[3] = 0xDEADBEEFu;
    in.gpr[1] = 0xCAFEBABEu;
    run_triplet(in, {0x0F, 0xC7, 0x0F});
}

TEST_F(InterpreterOracleContract, LockCmpxchg8bEqual) {
    State in = sse_state();
    put_u32(in, 0, 0xAAAAAAAAu);
    put_u32(in, 4, 0xBBBBBBBBu);
    in.gpr[0] = 0xAAAAAAAAu;
    in.gpr[2] = 0xBBBBBBBBu;
    in.gpr[3] = 0x11111111u;
    in.gpr[1] = 0x22222222u;
    run_triplet(in, {0xF0, 0x0F, 0xC7, 0x0F});
}

TEST_F(InterpreterOracleContract, ClflushEdi) {
    State in = sse_state();
    for (int b = 0; b < 16; ++b) in.mem[b] = static_cast<uint8_t>(0xA0 + b);
    run_triplet(in, {0x0F, 0xAE, 0x3F});  // CLFLUSH [EDI]
}

TEST_F(InterpreterOracleContract, AesencZeros) {
    State in = sse_state();
    fill_xmm(in, 0, 0);
    fill_xmm(in, 1, 0);
    run_triplet(in, {0x66, 0x0F, 0x38, 0xDC, 0xC1});  // AESENC XMM0, XMM1
}

TEST_F(InterpreterOracleContract, AesenclastZeros) {
    State in = sse_state();
    fill_xmm(in, 0, 0);
    fill_xmm(in, 1, 0);
    run_triplet(in, {0x66, 0x0F, 0x38, 0xDD, 0xC1});
}

TEST_F(InterpreterOracleContract, AesdecZeros) {
    State in = sse_state();
    fill_xmm(in, 0, 0);
    fill_xmm(in, 1, 0);
    run_triplet(in, {0x66, 0x0F, 0x38, 0xDE, 0xC1});
}

TEST_F(InterpreterOracleContract, AesdeclastZeros) {
    State in = sse_state();
    fill_xmm(in, 0, 0);
    fill_xmm(in, 1, 0);
    run_triplet(in, {0x66, 0x0F, 0x38, 0xDF, 0xC1});
}

TEST_F(InterpreterOracleContract, AesimcZeros) {
    State in = sse_state();
    fill_xmm(in, 0, 0xA5);
    fill_xmm(in, 1, 0);
    run_triplet(in, {0x66, 0x0F, 0x38, 0xDB, 0xC1});
}

TEST_F(InterpreterOracleContract, AeskeygenassistRcon0) {
    State in = sse_state();
    fill_xmm(in, 0, 0xFF);
    fill_xmm(in, 1, 0);
    run_triplet(in, {0x66, 0x0F, 0x3A, 0xDF, 0xC1, 0x00});
}

TEST_F(InterpreterOracleContract, PclmulqdqImm0) {
    State in = sse_state();
    fill_xmm(in, 0, 0);
    fill_xmm(in, 1, 0);
    in.xmm[0][0] = 3;
    in.xmm[1][0] = 3;
    run_triplet(in, {0x66, 0x0F, 0x3A, 0x44, 0xC1, 0x00});
}

TEST_F(InterpreterOracleContract, Crc32EaxEcx) {
    State in = sse_state();
    in.gpr[0] = 0;
    in.gpr[1] = 0x01020304u;
    run_triplet(in, {0xF2, 0x0F, 0x38, 0xF1, 0xC1});  // CRC32 EAX, ECX
}

TEST_F(InterpreterOracleContract, Movshdup) {
    State in = sse_state();
    put_f32_lane(in, 1, 0, 1.0f);
    put_f32_lane(in, 1, 1, 2.0f);
    put_f32_lane(in, 1, 2, 3.0f);
    put_f32_lane(in, 1, 3, 4.0f);
    run_triplet(in, {0xF3, 0x0F, 0x16, 0xC1});  // MOVSHDUP XMM0, XMM1
}

TEST_F(InterpreterOracleContract, Haddps) {
    State in = sse_state();
    put_f32_lane(in, 0, 0, 1.0f);
    put_f32_lane(in, 0, 1, 2.0f);
    put_f32_lane(in, 0, 2, 3.0f);
    put_f32_lane(in, 0, 3, 4.0f);
    put_f32_lane(in, 1, 0, 5.0f);
    put_f32_lane(in, 1, 1, 6.0f);
    put_f32_lane(in, 1, 2, 7.0f);
    put_f32_lane(in, 1, 3, 8.0f);
    run_triplet(in, {0xF2, 0x0F, 0x7C, 0xC1});  // HADDPS XMM0, XMM1
}

TEST_F(InterpreterOracleContract, LddquFromEdi) {
    State in = sse_state();
    for (int b = 0; b < 16; ++b) in.mem[b] = static_cast<uint8_t>(0x30 + b);
    fill_xmm(in, 0, 0);
    run_triplet(in, {0xF2, 0x0F, 0xF0, 0x07});  // LDDQU XMM0, [EDI]
}

TEST_F(InterpreterOracleContract, Phaddw) {
    State in = sse_state();
    for (int i = 0; i < 8; ++i) {
        const uint16_t a = static_cast<uint16_t>(i + 1);
        const uint16_t b = static_cast<uint16_t>(0x10 + i);
        std::memcpy(in.xmm[0].data() + i * 2, &a, 2);
        std::memcpy(in.xmm[1].data() + i * 2, &b, 2);
    }
    run_triplet(in, {0x66, 0x0F, 0x38, 0x01, 0xC1});  // PHADDW XMM0, XMM1
}

TEST_F(InterpreterOracleContract, PalignrImm1) {
    State in = sse_state();
    for (int b = 0; b < 16; ++b) {
        in.xmm[0][b] = static_cast<uint8_t>(0xA0 + b);
        in.xmm[1][b] = static_cast<uint8_t>(0xB0 + b);
    }
    run_triplet(in, {0x66, 0x0F, 0x3A, 0x0F, 0xC1, 0x01});
}

TEST_F(InterpreterOracleContract, PtestSetsZfCf) {
    State in = sse_state();
    fill_xmm(in, 0, 0xFF);
    fill_xmm(in, 1, 0);
    in.rflags = 0x202;
    run_triplet(in, {0x66, 0x0F, 0x38, 0x17, 0xC1});  // PTEST XMM0, XMM1
}

TEST_F(InterpreterOracleContract, Pcmpeqq) {
    State in = sse_state();
    fill_xmm(in, 0, 0);
    fill_xmm(in, 1, 0);
    in.xmm[0][0] = 1;
    in.xmm[1][0] = 1;
    in.xmm[0][8] = 2;
    in.xmm[1][8] = 3;
    run_triplet(in, {0x66, 0x0F, 0x38, 0x29, 0xC1});
}

TEST_F(InterpreterOracleContract, Pcmpgtq) {
    State in = sse_state();
    fill_xmm(in, 0, 0);
    fill_xmm(in, 1, 0);
    in.xmm[0][0] = 5;
    in.xmm[1][0] = 3;
    run_triplet(in, {0x66, 0x0F, 0x38, 0x37, 0xC1});
}

TEST_F(InterpreterOracleContract, BlendpsImm5) {
    State in = sse_state();
    put_f32_lane(in, 0, 0, 1.0f);
    put_f32_lane(in, 0, 1, 2.0f);
    put_f32_lane(in, 0, 2, 3.0f);
    put_f32_lane(in, 0, 3, 4.0f);
    put_f32_lane(in, 1, 0, 5.0f);
    put_f32_lane(in, 1, 1, 6.0f);
    put_f32_lane(in, 1, 2, 7.0f);
    put_f32_lane(in, 1, 3, 8.0f);
    run_triplet(in, {0x66, 0x0F, 0x3A, 0x0C, 0xC1, 0x05});
}

TEST_F(InterpreterOracleContract, RoundpsNearest) {
    State in = sse_state();
    put_f32_lane(in, 1, 0, 1.5f);
    put_f32_lane(in, 1, 1, 2.5f);
    put_f32_lane(in, 1, 2, -1.5f);
    put_f32_lane(in, 1, 3, 3.25f);
    fill_xmm(in, 0, 0);
    run_triplet(in, {0x66, 0x0F, 0x3A, 0x08, 0xC1, 0x00});
}

TEST_F(InterpreterOracleContract, Fld1ThenFxsave) {
    // FLD1 after FNINIT; compare ST0 plus SSE header, skip leftover empty ST
    // slots and FOP/FIP.
    State in = sse_state();
    CompareOpts opts;
    opts.mem_skip_off = vitreo::oracle::contract::kFxsavePtrOff;
    opts.mem_skip_len = vitreo::oracle::contract::kFxsavePtrLen;
    opts.mem_skip2_off = 42;   // reserved after ST0 through ST7
    opts.mem_skip2_len = 118;
    opts.mem_skip3_off = vitreo::oracle::contract::kFxsaveLegacyRsvdOff;
    opts.mem_skip3_len = vitreo::oracle::contract::kFxsaveLegacyRsvdLen;
    // FLD1 has no JIT stencil; interpreter is the x87 reference.
    run_pair(in, {0xD9, 0xE8, 0x0F, 0xAE, 0x07}, opts);  // FLD1; FXSAVE [EDI]
}

TEST_F(InterpreterOracleContract, AdcEaxEcx) {
    State in = sse_state();
    in.gpr[0] = 0xFFFFFFF0u;
    in.gpr[1] = 0x20u;
    in.rflags = 0x202 | 1;
    run_triplet(in, {0x11, 0xC8});
}

TEST_F(InterpreterOracleContract, SbbEaxEcx) {
    State in = sse_state();
    in.gpr[0] = 0x10u;
    in.gpr[1] = 0x20u;
    in.rflags = 0x202 | 1;
    run_triplet(in, {0x19, 0xC8});
}

TEST_F(InterpreterOracleContract, CmpEaxEcx) {
    State in = sse_state();
    in.gpr[0] = 5;
    in.gpr[1] = 7;
    run_triplet(in, {0x39, 0xC8});
}

TEST_F(InterpreterOracleContract, TestEaxEcx) {
    State in = sse_state();
    in.gpr[0] = 0xF0F0F0F0u;
    in.gpr[1] = 0x0F0F0F0Fu;
    run_triplet(in, {0x85, 0xC8});
}

TEST_F(InterpreterOracleContract, XaddEaxEcx) {
    State in = sse_state();
    in.gpr[0] = 0x11111111u;
    in.gpr[1] = 0x22222222u;
    run_triplet(in, {0x0F, 0xC1, 0xC1});
}

TEST_F(InterpreterOracleContract, CmpxchgEbxEcx) {
    State in = sse_state();
    in.gpr[0] = 0x11111111u;
    in.gpr[3] = 0x11111111u;
    in.gpr[1] = 0x22222222u;
    run_triplet(in, {0x0F, 0xB1, 0xCB});  // CMPXCHG EBX, ECX
}

TEST_F(InterpreterOracleContract, BtEaxEcx) {
    State in = sse_state();
    in.gpr[0] = 0xA5A5A5A5u;
    in.gpr[1] = 3;
    run_triplet(in, {0x0F, 0xA3, 0xC1});
}

TEST_F(InterpreterOracleContract, BsfEaxEcx) {
    State in = sse_state();
    in.gpr[1] = 0x00F00000u;
    CompareOpts opts;
    opts.flags_mask = 0x40;
    run_triplet(in, {0x0F, 0xBC, 0xC1}, opts);
}

TEST_F(InterpreterOracleContract, BsrEaxEcx) {
    State in = sse_state();
    in.gpr[1] = 0x00F00000u;
    CompareOpts opts;
    opts.flags_mask = 0x40;
    run_triplet(in, {0x0F, 0xBD, 0xC1}, opts);
}

TEST_F(InterpreterOracleContract, NegEax) {
    State in = sse_state();
    in.gpr[0] = 1;
    run_triplet(in, {0xF7, 0xD8});
}

TEST_F(InterpreterOracleContract, NotEax) {
    State in = sse_state();
    in.gpr[0] = 0xF0F0F0F0u;
    run_triplet(in, {0xF7, 0xD0});
}

TEST_F(InterpreterOracleContract, LeaEaxEcxDisp) {
    State in = sse_state();
    in.gpr[1] = 0x1000;
    run_triplet(in, {0x8D, 0x41, 0x10});
}

TEST_F(InterpreterOracleContract, MovzxEaxCl) {
    State in = sse_state();
    in.gpr[1] = 0xFFFFFF80u;
    run_triplet(in, {0x0F, 0xB6, 0xC1});
}

TEST_F(InterpreterOracleContract, MovsxEaxCl) {
    State in = sse_state();
    in.gpr[1] = 0x80;
    run_triplet(in, {0x0F, 0xBE, 0xC1});
}

TEST_F(InterpreterOracleContract, SetzAl) {
    State in = sse_state();
    in.gpr[0] = 0xFFFFFFFFu;
    in.rflags = 0x202 | 0x40;
    run_triplet(in, {0x0F, 0x94, 0xC0});
}

TEST_F(InterpreterOracleContract, RolEax1) {
    State in = sse_state();
    in.gpr[0] = 0x80000001u;
    run_triplet(in, {0xD1, 0xC0});
}

TEST_F(InterpreterOracleContract, BswapEax) {
    State in = sse_state();
    in.gpr[0] = 0x12345678u;
    run_triplet(in, {0x0F, 0xC8});
}

TEST_F(InterpreterOracleContract, ImulEaxEcx) {
    State in = sse_state();
    in.gpr[0] = 7;
    in.gpr[1] = 9;
    CompareOpts opts;
    opts.flags_mask = 0x801;  // CF|OF; SF/ZF/AF/PF undefined (SDM IMUL)
    run_triplet(in, {0x0F, 0xAF, 0xC1}, opts);
}

TEST_F(InterpreterOracleContract, AddpsXmm0Xmm1) {
    State in = sse_state();
    put_f32_lane(in, 0, 0, 1.0f);
    put_f32_lane(in, 0, 1, 2.0f);
    put_f32_lane(in, 1, 0, 3.0f);
    put_f32_lane(in, 1, 1, 4.0f);
    run_triplet(in, {0x0F, 0x58, 0xC1});
}

TEST_F(InterpreterOracleContract, MulsdXmm0Xmm1) {
    State in = sse_state();
    put_f64_lane(in, 0, 0, 1.5);
    put_f64_lane(in, 1, 0, 2.0);
    run_triplet(in, {0xF2, 0x0F, 0x59, 0xC1});
}

TEST_F(InterpreterOracleContract, ComissXmm0Xmm1) {
    State in = sse_state();
    put_f32_lane(in, 0, 0, 1.0f);
    put_f32_lane(in, 1, 0, 2.0f);
    run_triplet(in, {0x0F, 0x2F, 0xC1});
}

TEST_F(InterpreterOracleContract, AndpsXmm0Xmm1) {
    State in = sse_state();
    in.xmm[0].fill(0xFF);
    in.xmm[1].fill(0x0F);
    run_triplet(in, {0x0F, 0x54, 0xC1});
}

TEST_F(InterpreterOracleContract, ShufpsImm1b) {
    State in = sse_state();
    put_f32_lane(in, 0, 0, 1.0f);
    put_f32_lane(in, 0, 1, 2.0f);
    put_f32_lane(in, 0, 2, 3.0f);
    put_f32_lane(in, 0, 3, 4.0f);
    put_f32_lane(in, 1, 0, 5.0f);
    put_f32_lane(in, 1, 1, 6.0f);
    put_f32_lane(in, 1, 2, 7.0f);
    put_f32_lane(in, 1, 3, 8.0f);
    run_triplet(in, {0x0F, 0xC6, 0xC1, 0x1B});
}

TEST_F(InterpreterOracleContract, PadddXmm0Xmm1) {
    State in = sse_state();
    in.xmm[0].fill(0x01);
    in.xmm[1].fill(0x02);
    run_triplet(in, {0x66, 0x0F, 0xFE, 0xC1});
}

TEST_F(InterpreterOracleContract, PandXmm0Xmm1) {
    State in = sse_state();
    in.xmm[0].fill(0xFF);
    in.xmm[1].fill(0x0F);
    run_triplet(in, {0x66, 0x0F, 0xDB, 0xC1});
}

TEST_F(InterpreterOracleContract, PextrdEaxXmm0) {
    State in = sse_state();
    in.xmm[0] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    in.gpr[0] = 0;
    run_triplet(in, {0x66, 0x0F, 0x3A, 0x16, 0xC0, 0x01});
}

TEST_F(InterpreterOracleContract, PinsrdXmm0Eax) {
    State in = sse_state();
    in.gpr[0] = 0xAABBCCDDu;
    in.xmm[0].fill(0);
    run_triplet(in, {0x66, 0x0F, 0x3A, 0x22, 0xC0, 0x01});
}

TEST_F(InterpreterOracleContract, Pmovsxbw) {
    State in = sse_state();
    in.xmm[1] = {0x7F, 0x80, 0x01, 0xFF, 0x00, 0x40, 0xC0, 0x10,
                 0, 0, 0, 0, 0, 0, 0, 0};
    run_triplet(in, {0x66, 0x0F, 0x38, 0x20, 0xC1});
}

TEST_F(InterpreterOracleContract, Packusdw) {
    State in = sse_state();
    in.xmm[0].fill(0x01);
    in.xmm[1].fill(0x02);
    run_triplet(in, {0x66, 0x0F, 0x38, 0x2B, 0xC1});
}

TEST_F(InterpreterOracleContract, Pause) {
    run_triplet(sse_state(), {0xF3, 0x90});
}

}  // namespace

// A non-linear MOV r, [mem] between a carry producer and its ADC/SBB consumer.
// The load's two-region bounds check is a pair of SUBS and, unlike the store
// path, does not spill/fill NZCV -- so compile_arithmetic must NOT take its
// "directly preceding ADD leaves NZCV live" fast path across one. The register
// and store forms are here as controls: they genuinely do preserve NZCV, so a
// fix that simply stopped fusing everywhere would still pass this test.
TEST_F(InterpreterOracleContract, CarrySurvivesMovAcrossProducer) {
    struct Variant { const char* name; std::vector<uint8_t> code; };
    const std::vector<Variant> variants{
        {"add;adc", {0x03, 0xC1, 0x83, 0xD6, 0x00}},
        {"add;mov esi,edx;adc", {0x03, 0xC1, 0x8B, 0xF2, 0x83, 0xD6, 0x00}},
        {"add;mov ecx,[ebp+10];adc",
         {0x03, 0xC1, 0x8B, 0x4D, 0x10, 0x83, 0xD6, 0x00}},
        {"add;mov esi,edx;mov ecx,[ebp+10];adc",
         {0x03, 0xC1, 0x8B, 0xF2, 0x8B, 0x4D, 0x10, 0x83, 0xD6, 0x00}},
        {"add;lea ecx,[ebp+10];adc",
         {0x03, 0xC1, 0x8D, 0x4D, 0x10, 0x83, 0xD6, 0x00}},
        {"add;mov [ebp+10],ecx;adc",
         {0x03, 0xC1, 0x89, 0x4D, 0x10, 0x83, 0xD6, 0x00}},
    };

    // EAX + ECX = 0xFFFFFFFF + 1 carries; ADC ESI, 0 must see it.
    for (const auto& v : variants) {
        SCOPED_TRACE(v.name);
        State in = zero_state();
        in.gpr[0] = 0xFFFFFFFFu;  // EAX
        in.gpr[1] = 0x00000001u;  // ECX
        in.gpr[2] = 0x11111111u;  // EDX
        in.gpr[5] = vitreo::oracle::kMemVa + 4;  // EBP
        in.gpr[6] = 0x22222222u;  // ESI
        put_u32(in, 20, 0xAAAAAAAAu);  // [ebp+0x10]
        run_triplet(in, v.code);
    }
}
