// SPDX-License-Identifier: LGPL-2.1-only
// x86-64 interpreter/JIT ↔ Oracle contract smoke tests (CPU plan M1).
//
// Same shape as test_oracle_contract.cpp but in long mode: the snippet runs
// three ways — on the real CPU through the host x86 runner, on the Vitreo interpreter, and
// on the Vitreo JIT — and all three post-states must agree over 16 full-width
// GPRs, XMM0-15, RFLAGS and the shared scratch page.
//
// The snippet is loaded at oracle::kCodeVa64 on every side, which is what
// makes RIP-relative operands directly comparable.

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

class OracleContract64 : public ::testing::Test {
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

    // RIP-relative disp32 that makes an instruction ending `insn_len` bytes
    // into the snippet address `target`.
    static uint32_t rip_disp(uint64_t target, size_t insn_len) {
        const uint64_t next = vitreo::oracle::kCodeVa64 + insn_len;
        return static_cast<uint32_t>(static_cast<uint64_t>(target) - next);
    }

    // OF is architecturally *undefined* after a shift whose count is not 1
    // (Intel SDM Vol. 2B, SAL/SAR/SHL/SHR). Silicon still produces a value;
    // Vitreo deliberately leaves OF alone. Exclude it rather than encode a
    // non-contract into the gate.
    static constexpr uint32_t kFlagsNoOf =
        vitreo::oracle::contract::kArithFlagsMask & ~0x800u;

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
};

// --- REX.W arithmetic over the legacy 8 -------------------------------------

TEST_F(OracleContract64, AddRaxRcx) {
    run_triplet(seed_state(), {0x48, 0x01, 0xC8});  // add rax, rcx
}

TEST_F(OracleContract64, SubRaxRbx) {
    run_triplet(seed_state(), {0x48, 0x29, 0xD8});  // sub rax, rbx
}

TEST_F(OracleContract64, AddCarriesOutOf64Bits) {
    State s = seed_state();
    s.gpr[0] = 0xFFFF'FFFF'FFFF'FFFFull;
    s.gpr[1] = 1;
    run_triplet(s, {0x48, 0x01, 0xC8});  // add rax, rcx
}

TEST_F(OracleContract64, ImulRaxRcx) {
    run_triplet(seed_state(), {0x48, 0x0F, 0xAF, 0xC1});  // imul rax, rcx
}

// --- R8-R15 ------------------------------------------------------------------

TEST_F(OracleContract64, AddR8R9) {
    run_triplet(seed_state(), {0x4D, 0x01, 0xC8});  // add r8, r9
}

TEST_F(OracleContract64, XorR11R11) {
    run_triplet(seed_state(), {0x4D, 0x31, 0xDB});  // xor r11, r11
}

TEST_F(OracleContract64, MovR15R10) {
    run_triplet(seed_state(), {0x4D, 0x89, 0xD7});  // mov r15, r10
}

TEST_F(OracleContract64, BswapR8) {
    run_triplet(seed_state(), {0x49, 0x0F, 0xC8});  // bswap r8
}

TEST_F(OracleContract64, ShlRaxClMaskedCount) {
    // CL is RCX's low byte = 0x66 = 102, which the 64-bit form masks to 38
    // (SDM: counts are masked to 6 bits with REX.W). OF is undefined there.
    run_triplet(seed_state(), {0x48, 0xD3, 0xE0}, kFlagsNoOf);  // shl rax, cl
}

TEST_F(OracleContract64, ShrR13ClMaskedCount) {
    run_triplet(seed_state(), {0x49, 0xD3, 0xED}, kFlagsNoOf);  // shr r13, cl
}

TEST_F(OracleContract64, ShlRaxClCountOneDefinesOf) {
    State s = seed_state();
    s.gpr[1] = 1;  // CL = 1 -> OF is architecturally defined
    run_triplet(s, {0x48, 0xD3, 0xE0});  // shl rax, cl
}

TEST_F(OracleContract64, ShrR13ClCountOneDefinesOf) {
    State s = seed_state();
    s.gpr[1] = 1;
    run_triplet(s, {0x49, 0xD3, 0xED});  // shr r13, cl
}

TEST_F(OracleContract64, ShlRaxClCountZeroLeavesFlags) {
    // SDM: a masked count of 0 leaves all flags unaffected.
    State s = seed_state();
    s.gpr[1] = 64;  // masks to 0 under REX.W
    s.rflags = 0x202 | 0x8D5;
    run_triplet(s, {0x48, 0xD3, 0xE0});  // shl rax, cl
}

// --- 32-bit operand size in long mode zero-extends ---------------------------

TEST_F(OracleContract64, AddEaxEcxZeroExtends) {
    run_triplet(seed_state(), {0x01, 0xC8});  // add eax, ecx
}

TEST_F(OracleContract64, MovR9dR10dZeroExtends) {
    run_triplet(seed_state(), {0x45, 0x89, 0xD1});  // mov r9d, r10d
}

// --- RIP-relative addressing -------------------------------------------------

TEST_F(OracleContract64, RipRelativeLoad32) {
    // mov eax, [rip+disp32] -> kMemVa. Instruction is 6 bytes.
    std::vector<uint8_t> code = {0x8B, 0x05, 0, 0, 0, 0};
    const uint32_t d = rip_disp(vitreo::oracle::kMemVa, code.size());
    std::memcpy(code.data() + 2, &d, 4);
    run_triplet(seed_state(), code);
}

TEST_F(OracleContract64, RipRelativeLoad64) {
    // mov rax, [rip+disp32] -> kMemVa+8. Instruction is 7 bytes.
    std::vector<uint8_t> code = {0x48, 0x8B, 0x05, 0, 0, 0, 0};
    const uint32_t d = rip_disp(vitreo::oracle::kMemVa + 8, code.size());
    std::memcpy(code.data() + 3, &d, 4);
    run_triplet(seed_state(), code);
}

TEST_F(OracleContract64, RipRelativeStore64) {
    // mov [rip+disp32], rcx -> kMemVa+16. Instruction is 7 bytes.
    std::vector<uint8_t> code = {0x48, 0x89, 0x0D, 0, 0, 0, 0};
    const uint32_t d = rip_disp(vitreo::oracle::kMemVa + 16, code.size());
    std::memcpy(code.data() + 3, &d, 4);
    run_triplet(seed_state(), code);
}

// --- Memory operands through a 64-bit base ----------------------------------

TEST_F(OracleContract64, LoadThroughRdi) {
    run_triplet(seed_state(), {0x48, 0x8B, 0x07});  // mov rax, [rdi]
}

TEST_F(OracleContract64, StoreThroughRdi) {
    run_triplet(seed_state(), {0x48, 0x89, 0x47, 0x20});  // mov [rdi+0x20], rax
}

// --- XMM8-15 -----------------------------------------------------------------

TEST_F(OracleContract64, PaddqXmm8Xmm9) {
    run_triplet(seed_state(), {0x66, 0x45, 0x0F, 0xD4, 0xC1});  // paddq xmm8, xmm9
}

TEST_F(OracleContract64, RepeatedPaddqXmm8Xmm9) {
    // Intel SDM Vol. 2 PADDQ: each operation independently wraps both packed
    // qwords and leaves flags untouched. This also exercises Vitreo's
    // one-instruction x64 V0 forwarding against native Windows execution.
    run_triplet(seed_state(), {
        0x66, 0x45, 0x0F, 0xD4, 0xC1,
        0x66, 0x45, 0x0F, 0xD4, 0xC1,
        0x66, 0x45, 0x0F, 0xD4, 0xC1,
        0x66, 0x45, 0x0F, 0xD4, 0xC1,
    });
}

TEST_F(OracleContract64, PxorXmm15Xmm15) {
    run_triplet(seed_state(), {0x66, 0x45, 0x0F, 0xEF, 0xFF});  // pxor xmm15, xmm15
}

TEST_F(OracleContract64, MovdqaXmm12Xmm3) {
    run_triplet(seed_state(), {0x66, 0x44, 0x0F, 0x6F, 0xE3});  // movdqa xmm12, xmm3
}

// ============================================================================
// Campaign ISA vs the host x86 runner (long mode)
// ============================================================================

static void put_f32_lane64(State& s, int xmm, int lane, float v) {
    std::memcpy(s.xmm[xmm].data() + lane * 4, &v, 4);
}

static void put_f64_lane64(State& s, int xmm, int lane, double v) {
    std::memcpy(s.xmm[xmm].data() + lane * 8, &v, 8);
}

TEST_F(OracleContract64, ClcClearsCfOnly) {
    State s = seed_state();
    s.rflags = 0x202 | 0x8D5;
    run_triplet(s, {0xF8});  // clc
}

TEST_F(OracleContract64, StcSetsCfOnly) {
    State s = seed_state();
    s.rflags = 0x202 | 0x8D4;  // all arith except CF
    run_triplet(s, {0xF9});  // stc
}

TEST_F(OracleContract64, CmcTogglesCf) {
    State s = seed_state();
    s.rflags = 0x202 | 0x8D5;
    run_triplet(s, {0xF5});  // cmc
}

TEST_F(OracleContract64, PopcntRaxRcx) {
    State s = seed_state();
    s.gpr[1] = 0xF0F0F0F0F0F0F0F0ull;
    run_triplet(s, {0xF3, 0x48, 0x0F, 0xB8, 0xC1});  // popcnt rax, rcx
}

TEST_F(OracleContract64, LahfSahfLongMode) {
    State s = seed_state();
    s.rflags = 0x202 | 0x8D5;
    run_triplet(s, {0x9F, 0x9E});  // lahf; sahf
}

TEST_F(OracleContract64, PhminposuwXmm0Xmm1) {
    State s = seed_state();
    const uint16_t words[8] = {9, 8, 7, 1, 6, 5, 4, 3};
    std::memcpy(s.xmm[1].data(), words, 16);
    s.xmm[0].fill(0xFF);
    run_triplet(s, {0x66, 0x0F, 0x38, 0x41, 0xC1});
}

TEST_F(OracleContract64, PcmpistriEqualEach) {
    State s = seed_state();
    s.xmm[0].fill(0);
    s.xmm[1].fill(0);
    std::memcpy(s.xmm[0].data(), "Hello", 6);
    std::memcpy(s.xmm[1].data(), "Hello", 6);
    run_triplet(s, {0x66, 0x0F, 0x3A, 0x63, 0xC1, 0x08});
}

TEST_F(OracleContract64, PcmpistrmEqualEach) {
    State s = seed_state();
    s.xmm[0].fill(0);
    s.xmm[1].fill(0);
    s.xmm[2].fill(0);
    std::memcpy(s.xmm[1].data(), "Hello", 6);
    std::memcpy(s.xmm[2].data(), "Hello", 6);
    run_triplet(s, {0x66, 0x0F, 0x3A, 0x62, 0xCA, 0x08});
}

TEST_F(OracleContract64, PcmpestriExplicitLengthMsb) {
    State s = seed_state();
    s.xmm[0].fill('X');
    s.xmm[1].fill('Y');
    std::memcpy(s.xmm[0].data(), "AAA", 3);
    std::memcpy(s.xmm[1].data(), "AAA", 3);
    s.gpr[0] = 3;  // EAX
    s.gpr[2] = 3;  // EDX
    run_triplet(s, {0x66, 0x0F, 0x3A, 0x61, 0xC1, 0x48});
}

TEST_F(OracleContract64, DppsAllLanes) {
    State s = seed_state();
    put_f32_lane64(s, 0, 0, 1.0f);
    put_f32_lane64(s, 0, 1, 2.0f);
    put_f32_lane64(s, 0, 2, 3.0f);
    put_f32_lane64(s, 0, 3, 4.0f);
    put_f32_lane64(s, 1, 0, 5.0f);
    put_f32_lane64(s, 1, 1, 6.0f);
    put_f32_lane64(s, 1, 2, 7.0f);
    put_f32_lane64(s, 1, 3, 8.0f);
    run_triplet(s, {0x66, 0x0F, 0x3A, 0x40, 0xC1, 0xFF});
}

TEST_F(OracleContract64, DppdAllLanes) {
    State s = seed_state();
    put_f64_lane64(s, 0, 0, 1.0);
    put_f64_lane64(s, 0, 1, 2.0);
    put_f64_lane64(s, 1, 0, 3.0);
    put_f64_lane64(s, 1, 1, 4.0);
    run_triplet(s, {0x66, 0x0F, 0x3A, 0x41, 0xC1, 0x33});
}

TEST_F(OracleContract64, MpsadbwImm0) {
    State s = seed_state();
    for (int b = 0; b < 16; ++b) {
        s.xmm[0][b] = static_cast<uint8_t>(b + 1);
        s.xmm[1][b] = static_cast<uint8_t>(16 - b);
    }
    run_triplet(s, {0x66, 0x0F, 0x3A, 0x42, 0xC1, 0x00});
}

TEST_F(OracleContract64, PblendvbXmm1Xmm2) {
    State s = seed_state();
    s.xmm[0] = {0x80, 0x00, 0x80, 0x00, 0xFF, 0x7F, 0x00, 0x80,
                0x80, 0x80, 0x00, 0x00, 0x7F, 0x80, 0xFF, 0x00};
    s.xmm[1].fill(0x11);
    s.xmm[2].fill(0xAA);
    run_triplet(s, {0x66, 0x0F, 0x38, 0x10, 0xCA});
}

TEST_F(OracleContract64, MovntdqaFromRdi) {
    State s = seed_state();
    s.xmm[0].fill(0);
    run_triplet(s, {0x66, 0x0F, 0x38, 0x2A, 0x07});  // movntdqa xmm0, [rdi]
}

TEST_F(OracleContract64, LdmxcsrThenStmxcsr) {
    State s = seed_state();
    const uint32_t mx = 0x7F80u;
    std::memcpy(s.mem.data(), &mx, 4);
    run_triplet(s, {0x0F, 0xAE, 0x17, 0x0F, 0xAE, 0x5F, 0x04});
}

TEST_F(OracleContract64, Cmpxchg16bEqual) {
    // CMPXCHG16B [RDI]; RDI = kMemVa. RDX:RAX match mem; store RCX:RBX.
    State s = seed_state();
    const uint64_t lo = 0x1111111111111111ull;
    const uint64_t hi = 0x2222222222222222ull;
    std::memcpy(s.mem.data(), &lo, 8);
    std::memcpy(s.mem.data() + 8, &hi, 8);
    s.gpr[0] = lo;                         // RAX
    s.gpr[2] = hi;                         // RDX
    s.gpr[3] = 0x3333333333333333ull;      // RBX
    s.gpr[1] = 0x4444444444444444ull;      // RCX
    s.rflags = 0x202 | 0x8D5;
    run_triplet(s, {0x48, 0x0F, 0xC7, 0x0F});
}

TEST_F(OracleContract64, Cmpxchg16bNotEqual) {
    State s = seed_state();
    const uint64_t lo = 0xAAAAAAAAAAAAAAAAULL;
    const uint64_t hi = 0xBBBBBBBBBBBBBBBBULL;
    std::memcpy(s.mem.data(), &lo, 8);
    std::memcpy(s.mem.data() + 8, &hi, 8);
    s.gpr[0] = 1;
    s.gpr[2] = 2;
    s.gpr[3] = 3;
    s.gpr[1] = 4;
    s.rflags = 0x202 | 0x8D5;
    run_triplet(s, {0x48, 0x0F, 0xC7, 0x0F});
}

TEST_F(OracleContract64, LockCmpxchg16bEqual) {
    State s = seed_state();
    const uint64_t lo = 0x1111111111111111ull;
    const uint64_t hi = 0x2222222222222222ull;
    std::memcpy(s.mem.data(), &lo, 8);
    std::memcpy(s.mem.data() + 8, &hi, 8);
    s.gpr[0] = lo;
    s.gpr[2] = hi;
    s.gpr[3] = 0x3333333333333333ull;
    s.gpr[1] = 0x4444444444444444ull;
    s.rflags = 0x202 | 0x8D5;
    run_triplet(s, {0xF0, 0x48, 0x0F, 0xC7, 0x0F});
}

TEST_F(OracleContract64, Fxsave64Rdi) {
    State s = seed_state();
    CompareOpts opts;
    opts.mem_skip_off = vitreo::oracle::contract::kFxsavePtrOff;
    opts.mem_skip_len = vitreo::oracle::contract::kFxsavePtrLen;
    opts.mem_skip2_off = vitreo::oracle::contract::kFxsaveStOff;
    opts.mem_skip2_len = vitreo::oracle::contract::kFxsaveStLen;
    opts.mem_skip3_off = vitreo::oracle::contract::kFxsave64RsvdOff;
    opts.mem_skip3_len = vitreo::oracle::contract::kFxsave64RsvdLen;
    run_triplet(s, {0x48, 0x0F, 0xAE, 0x07}, opts);  // fxsave64 [rdi]
}

TEST_F(OracleContract64, Fxrstor64Rdi) {
    State s = seed_state();
    s.mem.fill(0);
    s.mem[0] = 0x7F;
    s.mem[1] = 0x03;
    const uint32_t mx = 0x1F80u;
    const uint32_t mask = 0xFFFFu;
    std::memcpy(s.mem.data() + 24, &mx, 4);
    std::memcpy(s.mem.data() + 28, &mask, 4);
    for (int r = 0; r < 16; ++r) {
        for (int b = 0; b < 16; ++b) {
            s.mem[160 + r * 16 + b] = static_cast<uint8_t>(0x20 * r + b);
        }
    }
    run_triplet(s, {0x48, 0x0F, 0xAE, 0x0F});  // fxrstor64 [rdi]
}

TEST_F(OracleContract64, RdtscLeavesOtherState) {
    State s = seed_state();
    CompareOpts opts;
    opts.gpr_ignore_mask = vitreo::oracle::contract::kGprRax |
                           vitreo::oracle::contract::kGprRdx;
    run_triplet(s, {0x0F, 0x31}, opts);
}

TEST_F(OracleContract64, RdtscpLeavesOtherState) {
    State s = seed_state();
    CompareOpts opts;
    opts.gpr_ignore_mask = vitreo::oracle::contract::kGprRax |
                           vitreo::oracle::contract::kGprRcx |
                           vitreo::oracle::contract::kGprRdx;
    run_triplet(s, {0x0F, 0x01, 0xF9}, opts);
}

TEST_F(OracleContract64, BlendvpsXmm1Xmm2) {
    State s = seed_state();
    s.xmm[0] = {0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00};
    s.xmm[1].fill(0x11);
    s.xmm[2].fill(0xAA);
    run_triplet(s, {0x66, 0x0F, 0x38, 0x14, 0xCA});
}

TEST_F(OracleContract64, BlendvpdXmm1Xmm2) {
    State s = seed_state();
    s.xmm[0] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    s.xmm[1].fill(0x11);
    s.xmm[2].fill(0xAA);
    run_triplet(s, {0x66, 0x0F, 0x38, 0x15, 0xCA});
}

TEST_F(OracleContract64, CmovzTaken) {
    State s = seed_state();
    s.gpr[0] = 0x1111111111111111ull;
    s.gpr[1] = 0x2222222222222222ull;
    s.rflags = 0x202 | 0x40;
    run_triplet(s, {0x48, 0x0F, 0x44, 0xC1});  // cmovz rax, rcx
}

TEST_F(OracleContract64, CmovzNotTaken) {
    State s = seed_state();
    s.gpr[0] = 0x1111111111111111ull;
    s.gpr[1] = 0x2222222222222222ull;
    s.rflags = 0x202;
    run_triplet(s, {0x48, 0x0F, 0x44, 0xC1});
}

TEST_F(OracleContract64, Cmpxchg8bEqual) {
    State s = seed_state();
    const uint32_t lo = 0xAAAAAAAA;
    const uint32_t hi = 0xBBBBBBBB;
    std::memcpy(s.mem.data(), &lo, 4);
    std::memcpy(s.mem.data() + 4, &hi, 4);
    s.gpr[0] = lo;
    s.gpr[2] = hi;
    s.gpr[3] = 0x11111111u;
    s.gpr[1] = 0x22222222u;
    s.rflags = 0x202 | 0x8D5;
    run_triplet(s, {0x0F, 0xC7, 0x0F});
}

TEST_F(OracleContract64, ClflushRdi) {
    State s = seed_state();
    run_triplet(s, {0x0F, 0xAE, 0x3F});
}

TEST_F(OracleContract64, AesencXmm0Xmm1) {
    State s = seed_state();
    s.xmm[0].fill(0);
    s.xmm[1].fill(0);
    run_triplet(s, {0x66, 0x0F, 0x38, 0xDC, 0xC1});
}

TEST_F(OracleContract64, AesdecXmm0Xmm1) {
    State s = seed_state();
    s.xmm[0].fill(0);
    s.xmm[1].fill(0);
    run_triplet(s, {0x66, 0x0F, 0x38, 0xDE, 0xC1});
}

TEST_F(OracleContract64, AeskeygenassistRcon0) {
    State s = seed_state();
    s.xmm[0].fill(0xFF);
    s.xmm[1].fill(0);
    run_triplet(s, {0x66, 0x0F, 0x3A, 0xDF, 0xC1, 0x00});
}

TEST_F(OracleContract64, PclmulqdqImm0) {
    State s = seed_state();
    s.xmm[0].fill(0);
    s.xmm[1].fill(0);
    s.xmm[0][0] = 3;
    s.xmm[1][0] = 3;
    run_triplet(s, {0x66, 0x0F, 0x3A, 0x44, 0xC1, 0x00});
}

TEST_F(OracleContract64, Crc32RaxRcx) {
    State s = seed_state();
    s.gpr[0] = 0;
    s.gpr[1] = 0x01020304ull;
    run_triplet(s, {0xF2, 0x48, 0x0F, 0x38, 0xF1, 0xC1});
}

TEST_F(OracleContract64, Movshdup) {
    State s = seed_state();
    put_f32_lane64(s, 1, 0, 1.0f);
    put_f32_lane64(s, 1, 1, 2.0f);
    put_f32_lane64(s, 1, 2, 3.0f);
    put_f32_lane64(s, 1, 3, 4.0f);
    run_triplet(s, {0xF3, 0x0F, 0x16, 0xC1});
}

TEST_F(OracleContract64, Haddps) {
    State s = seed_state();
    put_f32_lane64(s, 0, 0, 1.0f);
    put_f32_lane64(s, 0, 1, 2.0f);
    put_f32_lane64(s, 0, 2, 3.0f);
    put_f32_lane64(s, 0, 3, 4.0f);
    put_f32_lane64(s, 1, 0, 5.0f);
    put_f32_lane64(s, 1, 1, 6.0f);
    put_f32_lane64(s, 1, 2, 7.0f);
    put_f32_lane64(s, 1, 3, 8.0f);
    run_triplet(s, {0xF2, 0x0F, 0x7C, 0xC1});
}

TEST_F(OracleContract64, LddquFromRdi) {
    State s = seed_state();
    s.xmm[0].fill(0);
    run_triplet(s, {0xF2, 0x0F, 0xF0, 0x07});
}

TEST_F(OracleContract64, Phaddw) {
    State s = seed_state();
    for (int i = 0; i < 8; ++i) {
        const uint16_t a = static_cast<uint16_t>(i + 1);
        const uint16_t b = static_cast<uint16_t>(0x10 + i);
        std::memcpy(s.xmm[0].data() + i * 2, &a, 2);
        std::memcpy(s.xmm[1].data() + i * 2, &b, 2);
    }
    run_triplet(s, {0x66, 0x0F, 0x38, 0x01, 0xC1});
}

TEST_F(OracleContract64, PalignrImm1) {
    State s = seed_state();
    for (int b = 0; b < 16; ++b) {
        s.xmm[0][b] = static_cast<uint8_t>(0xA0 + b);
        s.xmm[1][b] = static_cast<uint8_t>(0xB0 + b);
    }
    run_triplet(s, {0x66, 0x0F, 0x3A, 0x0F, 0xC1, 0x01});
}

TEST_F(OracleContract64, PtestSetsZfCf) {
    State s = seed_state();
    s.xmm[0].fill(0xFF);
    s.xmm[1].fill(0);
    s.rflags = 0x202;
    run_triplet(s, {0x66, 0x0F, 0x38, 0x17, 0xC1});
}

TEST_F(OracleContract64, Pcmpeqq) {
    State s = seed_state();
    s.xmm[0].fill(0);
    s.xmm[1].fill(0);
    s.xmm[0][0] = 1;
    s.xmm[1][0] = 1;
    s.xmm[0][8] = 2;
    s.xmm[1][8] = 3;
    run_triplet(s, {0x66, 0x0F, 0x38, 0x29, 0xC1});
}

TEST_F(OracleContract64, Pcmpgtq) {
    State s = seed_state();
    s.xmm[0].fill(0);
    s.xmm[1].fill(0);
    s.xmm[0][0] = 5;
    s.xmm[1][0] = 3;
    run_triplet(s, {0x66, 0x0F, 0x38, 0x37, 0xC1});
}

TEST_F(OracleContract64, BlendpsImm5) {
    State s = seed_state();
    put_f32_lane64(s, 0, 0, 1.0f);
    put_f32_lane64(s, 0, 1, 2.0f);
    put_f32_lane64(s, 0, 2, 3.0f);
    put_f32_lane64(s, 0, 3, 4.0f);
    put_f32_lane64(s, 1, 0, 5.0f);
    put_f32_lane64(s, 1, 1, 6.0f);
    put_f32_lane64(s, 1, 2, 7.0f);
    put_f32_lane64(s, 1, 3, 8.0f);
    run_triplet(s, {0x66, 0x0F, 0x3A, 0x0C, 0xC1, 0x05});
}

TEST_F(OracleContract64, RoundpsNearest) {
    State s = seed_state();
    put_f32_lane64(s, 1, 0, 1.5f);
    put_f32_lane64(s, 1, 1, 2.5f);
    put_f32_lane64(s, 1, 2, -1.5f);
    put_f32_lane64(s, 1, 3, 3.25f);
    s.xmm[0].fill(0);
    run_triplet(s, {0x66, 0x0F, 0x3A, 0x08, 0xC1, 0x00});
}

TEST_F(OracleContract64, PushRax) {
    State s = seed_state();
    s.gpr[4] = vitreo::oracle::kMemVa + 0x100;
    s.gpr[0] = 0x1122334455667788ull;
    CompareOpts opts;
    opts.compare_rsp = true;
    run_triplet(s, {0x50}, opts);  // push rax
}

TEST_F(OracleContract64, PopRax) {
    State s = seed_state();
    s.gpr[4] = vitreo::oracle::kMemVa + 0xF8;
    const uint64_t v = 0xA1B2C3D4E5F60718ull;
    std::memcpy(s.mem.data() + 0xF8, &v, 8);
    s.gpr[0] = 0;
    CompareOpts opts;
    opts.compare_rsp = true;
    run_triplet(s, {0x58}, opts);  // pop rax
}

TEST_F(OracleContract64, PushImm32) {
    State s = seed_state();
    s.gpr[4] = vitreo::oracle::kMemVa + 0x100;
    CompareOpts opts;
    opts.compare_rsp = true;
    run_triplet(s, {0x68, 0x78, 0x56, 0x34, 0x12}, opts);  // push 0x12345678
}

TEST_F(OracleContract64, CallRel32Fallthrough) {
    State s = seed_state();
    s.gpr[4] = vitreo::oracle::kMemVa + 0x100;
    CompareOpts opts;
    opts.compare_rsp = true;
    run_triplet(s, {0xE8, 0x00, 0x00, 0x00, 0x00}, opts);  // call $+5
}

TEST_F(OracleContract64, Leave) {
    State s = seed_state();
    s.gpr[5] = vitreo::oracle::kMemVa + 0xF8;  // RBP
    s.gpr[4] = vitreo::oracle::kMemVa + 0x40;  // RSP
    const uint64_t old_bp = 0x0102030405060708ull;
    std::memcpy(s.mem.data() + 0xF8, &old_bp, 8);
    CompareOpts opts;
    opts.compare_rsp = true;
    run_triplet(s, {0xC9}, opts);  // leave
}

TEST_F(OracleContract64, RetNear) {
    State s = seed_state();
    s.gpr[4] = vitreo::oracle::kMemVa + 0xF8;
    const uint64_t ret_to = vitreo::oracle::kCodeVa64 + 1;  // fall into epilogue
    std::memcpy(s.mem.data() + 0xF8, &ret_to, 8);
    CompareOpts opts;
    opts.compare_rsp = true;
    run_triplet(s, {0xC3}, opts);  // ret
}

TEST_F(OracleContract64, Pushfq) {
    State s = seed_state();
    s.gpr[4] = vitreo::oracle::kMemVa + 0x100;
    s.rflags = 0x202 | 0x8D5;
    CompareOpts opts;
    opts.compare_rsp = true;
    run_triplet(s, {0x9C}, opts);
}

TEST_F(OracleContract64, AdcRaxRcx) {
    State s = seed_state();
    s.rflags = 0x202 | 1;  // CF
    run_triplet(s, {0x48, 0x11, 0xC8});  // adc rax, rcx
}

TEST_F(OracleContract64, SbbRaxRcx) {
    State s = seed_state();
    s.rflags = 0x202 | 1;
    run_triplet(s, {0x48, 0x19, 0xC8});
}

TEST_F(OracleContract64, CmpRaxRcx) {
    run_triplet(seed_state(), {0x48, 0x39, 0xC8});
}

TEST_F(OracleContract64, TestRaxRcx) {
    run_triplet(seed_state(), {0x48, 0x85, 0xC8});
}

TEST_F(OracleContract64, XaddRaxRcx) {
    run_triplet(seed_state(), {0x48, 0x0F, 0xC1, 0xC8});
}

TEST_F(OracleContract64, CmpxchgRbxRcx) {
    State s = seed_state();
    s.gpr[3] = s.gpr[0];  // RBX == RAX → exchange
    run_triplet(s, {0x48, 0x0F, 0xB1, 0xCB});  // cmpxchg rbx, rcx
}

TEST_F(OracleContract64, BtRaxRcx) {
    State s = seed_state();
    s.gpr[1] = 5;
    run_triplet(s, {0x48, 0x0F, 0xA3, 0xC8});  // bt rax, rcx
}

TEST_F(OracleContract64, BsfRaxRcx) {
    State s = seed_state();
    s.gpr[1] = 0x0000000000F00000ull;
    CompareOpts opts;
    opts.flags_mask = 0x40;  // ZF only; other flags undefined
    run_triplet(s, {0x48, 0x0F, 0xBC, 0xC1}, opts);
}

TEST_F(OracleContract64, BsrRaxRcx) {
    State s = seed_state();
    s.gpr[1] = 0x0000000000F00000ull;
    CompareOpts opts;
    opts.flags_mask = 0x40;
    run_triplet(s, {0x48, 0x0F, 0xBD, 0xC1}, opts);
}

TEST_F(OracleContract64, NegRax) {
    run_triplet(seed_state(), {0x48, 0xF7, 0xD8});
}

TEST_F(OracleContract64, NotRax) {
    run_triplet(seed_state(), {0x48, 0xF7, 0xD0});
}

TEST_F(OracleContract64, LeaRaxRcxDisp) {
    run_triplet(seed_state(), {0x48, 0x8D, 0x41, 0x10});  // lea rax, [rcx+0x10]
}

TEST_F(OracleContract64, MovzxRaxCl) {
    run_triplet(seed_state(), {0x48, 0x0F, 0xB6, 0xC1});
}

TEST_F(OracleContract64, MovsxRaxCl) {
    State s = seed_state();
    s.gpr[1] = 0x80;
    run_triplet(s, {0x48, 0x0F, 0xBE, 0xC1});
}

TEST_F(OracleContract64, SetzAl) {
    State s = seed_state();
    s.rflags = 0x202 | 0x40;
    run_triplet(s, {0x0F, 0x94, 0xC0});
}

TEST_F(OracleContract64, RolRax1) {
    run_triplet(seed_state(), {0x48, 0xD1, 0xC0});
}

TEST_F(OracleContract64, AddpsXmm0Xmm1) {
    State s = seed_state();
    put_f32_lane64(s, 0, 0, 1.0f);
    put_f32_lane64(s, 0, 1, 2.0f);
    put_f32_lane64(s, 0, 2, 3.0f);
    put_f32_lane64(s, 0, 3, 4.0f);
    put_f32_lane64(s, 1, 0, 5.0f);
    put_f32_lane64(s, 1, 1, 6.0f);
    put_f32_lane64(s, 1, 2, 7.0f);
    put_f32_lane64(s, 1, 3, 8.0f);
    run_triplet(s, {0x0F, 0x58, 0xC1});
}

TEST_F(OracleContract64, MulsdXmm0Xmm1) {
    State s = seed_state();
    put_f64_lane64(s, 0, 0, 1.5);
    put_f64_lane64(s, 1, 0, 2.0);
    run_triplet(s, {0xF2, 0x0F, 0x59, 0xC1});
}

TEST_F(OracleContract64, ComissXmm0Xmm1) {
    State s = seed_state();
    put_f32_lane64(s, 0, 0, 1.0f);
    put_f32_lane64(s, 1, 0, 2.0f);
    run_triplet(s, {0x0F, 0x2F, 0xC1});
}

TEST_F(OracleContract64, AndpsXmm0Xmm1) {
    run_triplet(seed_state(), {0x0F, 0x54, 0xC1});
}

TEST_F(OracleContract64, ShufpsImm1b) {
    run_triplet(seed_state(), {0x0F, 0xC6, 0xC1, 0x1B});
}

TEST_F(OracleContract64, PadddXmm0Xmm1) {
    run_triplet(seed_state(), {0x66, 0x0F, 0xFE, 0xC1});
}

TEST_F(OracleContract64, PandXmm0Xmm1) {
    run_triplet(seed_state(), {0x66, 0x0F, 0xDB, 0xC1});
}

TEST_F(OracleContract64, PextrdEaxXmm0) {
    State s = seed_state();
    s.gpr[0] = 0xFFFFFFFFFFFFFFFFULL;
    run_triplet(s, {0x66, 0x0F, 0x3A, 0x16, 0xC0, 0x01});
}

TEST_F(OracleContract64, PinsrdXmm0Eax) {
    State s = seed_state();
    s.gpr[0] = 0xAABBCCDDull;
    run_triplet(s, {0x66, 0x0F, 0x3A, 0x22, 0xC0, 0x01});
}

TEST_F(OracleContract64, Pmovsxbw) {
    State s = seed_state();
    s.xmm[1] = {0x7F, 0x80, 0x01, 0xFF, 0x00, 0x40, 0xC0, 0x10,
                0, 0, 0, 0, 0, 0, 0, 0};
    run_triplet(s, {0x66, 0x0F, 0x38, 0x20, 0xC1});
}

TEST_F(OracleContract64, Packusdw) {
    run_triplet(seed_state(), {0x66, 0x0F, 0x38, 0x2B, 0xC1});
}

TEST_F(OracleContract64, Pause) {
    run_triplet(seed_state(), {0xF3, 0x90});
}

TEST_F(OracleContract64, JmpRipRelIatThunk) {
    // FF 25 disp32 — the x64 IAT thunk: jmp qword ptr [rip+disp].
    // Plant a fall-through target at kMemVa so both sides take the same edge.
    State s = seed_state();
    const uint64_t insn_len = 6;
    const uint64_t target = vitreo::oracle::kCodeVa64 + insn_len;
    std::memcpy(s.mem.data(), &target, 8);
    std::vector<uint8_t> code{0xFF, 0x25};
    const uint32_t d = rip_disp(vitreo::oracle::kMemVa, code.size() + 4);
    code.push_back(static_cast<uint8_t>(d));
    code.push_back(static_cast<uint8_t>(d >> 8));
    code.push_back(static_cast<uint8_t>(d >> 16));
    code.push_back(static_cast<uint8_t>(d >> 24));
    run_triplet(s, code);
}

}  // namespace
