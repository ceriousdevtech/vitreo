// SPDX-License-Identifier: LGPL-2.1-only
// x86-64 interpreter/JIT ↔ Oracle integer ISA tests (remaining 64-bit ops).
//
// Same harness as test_oracle_contract_64.cpp. Fixture is OracleInteger64 so
// this TU does not ODR-collide with OracleContract64.

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

class OracleInteger64 : public ::testing::Test {
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

    // CMOVcc RAX, RCX — 48 0F 4x C1. Taken; RAX/RCX stay at seed values.
    static void cmov64_taken(uint8_t cc, uint64_t rflags) {
        State s = seed_state();
        s.rflags = rflags;
        run_triplet(s, {0x48, 0x0F, cc, 0xC1});
    }

    // SETcc AL — 0F 9x C0. Seed RAX has bits 8..63 set; SET must preserve them.
    static void setcc_al(uint8_t cc, uint64_t rflags) {
        State s = seed_state();
        s.rflags = rflags;
        run_triplet(s, {0x0F, cc, 0xC0});
    }
};

TEST_F(OracleInteger64, CmovbTaken) { cmov64_taken(0x42, 0x202 | 0x001); }   // CF
TEST_F(OracleInteger64, CmovnbTaken) { cmov64_taken(0x43, 0x202); }          // !CF
TEST_F(OracleInteger64, CmovnzTaken) { cmov64_taken(0x45, 0x202); }          // !ZF
TEST_F(OracleInteger64, CmovbeTaken) { cmov64_taken(0x46, 0x202 | 0x001); }  // CF
TEST_F(OracleInteger64, CmovnbeTaken) { cmov64_taken(0x47, 0x202); }         // !CF && !ZF
TEST_F(OracleInteger64, CmovsTaken) { cmov64_taken(0x48, 0x202 | 0x080); }   // SF
TEST_F(OracleInteger64, CmovnsTaken) { cmov64_taken(0x49, 0x202); }          // !SF
TEST_F(OracleInteger64, CmovlTaken) { cmov64_taken(0x4C, 0x202 | 0x080); }   // SF != OF
TEST_F(OracleInteger64, CmovnlTaken) { cmov64_taken(0x4D, 0x202); }          // SF == OF
TEST_F(OracleInteger64, CmovleTaken) { cmov64_taken(0x4E, 0x202 | 0x040); }  // ZF
TEST_F(OracleInteger64, CmovnleTaken) { cmov64_taken(0x4F, 0x202); }         // !ZF && SF==OF
TEST_F(OracleInteger64, CmovoTaken) { cmov64_taken(0x40, 0x202 | 0x800); }   // OF
TEST_F(OracleInteger64, CmovnoTaken) { cmov64_taken(0x41, 0x202); }          // !OF
TEST_F(OracleInteger64, CmovpTaken) { cmov64_taken(0x4A, 0x202 | 0x004); }   // PF
TEST_F(OracleInteger64, CmovnpTaken) { cmov64_taken(0x4B, 0x202); }          // !PF

TEST_F(OracleInteger64, SetnzAl) { setcc_al(0x95, 0x202); }           // !ZF
TEST_F(OracleInteger64, SetbAl) { setcc_al(0x92, 0x202 | 0x001); }    // CF
TEST_F(OracleInteger64, SetsAl) { setcc_al(0x98, 0x202 | 0x080); }    // SF
TEST_F(OracleInteger64, SetlAl) { setcc_al(0x9C, 0x202 | 0x080); }    // SF ^ OF
TEST_F(OracleInteger64, SetoAl) { setcc_al(0x90, 0x202 | 0x800); }    // OF
TEST_F(OracleInteger64, SetpAl) { setcc_al(0x9A, 0x202 | 0x004); }    // PF

// JZ rel8 + XOR RAX,RAX. rel8=3 skips the 3-byte REX.W XOR (48 31 C0).
TEST_F(OracleInteger64, JzTakenSkipsXor) {
    State s = seed_state();
    s.rflags = 0x202 | 0x040;
    run_triplet(s, {0x74, 0x03, 0x48, 0x31, 0xC0});
}

TEST_F(OracleInteger64, JzFallthroughXorsRax) {
    State s = seed_state();
    s.rflags = 0x202;  // ZF clear
    run_triplet(s, {0x74, 0x03, 0x48, 0x31, 0xC0});
}

TEST_F(OracleInteger64, JnzTakenSkipsXor) {
    State s = seed_state();
    s.rflags = 0x202;
    run_triplet(s, {0x75, 0x03, 0x48, 0x31, 0xC0});
}

TEST_F(OracleInteger64, JnzFallthroughXorsRax) {
    State s = seed_state();
    s.rflags = 0x202 | 0x040;
    run_triplet(s, {0x75, 0x03, 0x48, 0x31, 0xC0});
}

TEST_F(OracleInteger64, JmpRel8SkipsXor) {
    run_triplet(seed_state(), {0xEB, 0x03, 0x48, 0x31, 0xC0});
}

// Intel SDM Vol. 2 Jcc: near rel32 0F 80+cc. disp=0 is a no-op jump.
TEST_F(OracleInteger64, JccRel32Plus0) {
    for (uint8_t cc = 0x80; cc <= 0x8F; ++cc) {
        run_triplet(seed_state(), {0x0F, cc, 0x00, 0x00, 0x00, 0x00});
    }
}

// JO rel8 over INT3. OF set ⇒ taken; fall-through would #BP.
TEST_F(OracleInteger64, JoRel8TakenSkipsInt3) {
    State s = seed_state();
    s.rflags = 0x202 | 0x800;
    run_triplet(s, {0x70, 0x01, 0xCC});
}

// Intel SDM Vol. 2 LOOP/LOOPcc: decrement RCX (AddressSize=64), flags
// unchanged. disp=0 ⇒ RIP is the fall-through whether taken or not.
TEST_F(OracleInteger64, LoopRel8Plus0DecrementsRcx) {
    run_triplet(seed_state(), {0xE2, 0x00});
}

TEST_F(OracleInteger64, LoopRel8Plus0FromZero) {
    State s = seed_state();
    s.gpr[1] = 0;  // RCX wraps to ~0; a 32-bit decrement would zero-extend
    run_triplet(s, {0xE2, 0x00});
}

TEST_F(OracleInteger64, LoopeRel8Plus0DecrementsRcx) {
    State s = seed_state();
    s.rflags = 0x202 | 0x040;  // ZF
    run_triplet(s, {0xE1, 0x00});
}

TEST_F(OracleInteger64, LoopneRel8Plus0DecrementsRcx) {
    State s = seed_state();
    s.rflags = 0x202;  // ZF clear
    run_triplet(s, {0xE0, 0x00});
}

// Intel SDM Vol. 2 JRCXZ: jump if RCX==0. disp=0; RCX is not written.
TEST_F(OracleInteger64, JrcxzRel8Plus0) {
    run_triplet(seed_state(), {0xE3, 0x00});
}

TEST_F(OracleInteger64, JrcxzRel8Plus0WhenRcxZero) {
    State s = seed_state();
    s.gpr[1] = 0;
    run_triplet(s, {0xE3, 0x00});
}

// ENTER/LEAVE: oracle loads RSP only when gpr[4] != 0.
TEST_F(OracleInteger64, Enter8Level0) {
    State s = seed_state();
    s.gpr[4] = vitreo::oracle::kMemVa + 0x100;
    CompareOpts opts;
    opts.compare_rsp = true;
    run_triplet(s, {0xC8, 0x08, 0x00, 0x00}, opts);
}

TEST_F(OracleInteger64, LeaveRestoresFrame) {
    State s = seed_state();
    s.gpr[4] = vitreo::oracle::kMemVa + 0x100;
    s.gpr[5] = vitreo::oracle::kMemVa + 0x80;
    const uint64_t saved = 0x0000111122223333ull;
    std::memcpy(s.mem.data() + 0x80, &saved, 8);
    CompareOpts opts;
    opts.compare_rsp = true;
    run_triplet(s, {0xC9}, opts);
}

// SHLD/SHRD r/m64, r64, imm8. Count != 1 ⇒ OF undefined.
TEST_F(OracleInteger64, ShldRaxRcxImm4) {
    run_triplet(seed_state(), {0x48, 0x0F, 0xA4, 0xC8, 0x04}, kFlagsNoOf);
}

TEST_F(OracleInteger64, ShrdRaxRcxImm4) {
    run_triplet(seed_state(), {0x48, 0x0F, 0xAC, 0xC8, 0x04}, kFlagsNoOf);
}

// BTS/BTR/BTC r/m64, r64. ModRM C8 = reg=RCX, rm=RAX.
TEST_F(OracleInteger64, BtsRaxRcx) {
    run_triplet(seed_state(), {0x48, 0x0F, 0xAB, 0xC8});
}

TEST_F(OracleInteger64, BtrRaxRcx) {
    run_triplet(seed_state(), {0x48, 0x0F, 0xB3, 0xC8});
}

TEST_F(OracleInteger64, BtcRaxRcx) {
    run_triplet(seed_state(), {0x48, 0x0F, 0xBB, 0xC8});
}

TEST_F(OracleInteger64, BtRaxImm5) {
    run_triplet(seed_state(), {0x48, 0x0F, 0xBA, 0xE0, 0x05});
}

TEST_F(OracleInteger64, XchgRaxRcx) {
    run_triplet(seed_state(), {0x48, 0x91});
}

TEST_F(OracleInteger64, Cqo) {
    State s = seed_state();
    s.gpr[0] |= 0x8000'0000'0000'0000ull;  // RAX high bit → RDX = all ones
    run_triplet(s, {0x48, 0x99});
}

TEST_F(OracleInteger64, Cdqe) {
    State s = seed_state();
    s.gpr[0] = (s.gpr[0] & ~0xFFFFFFFFull) | 0x80000000ull;  // EAX = 0x80000000
    run_triplet(s, {0x48, 0x98});
}

TEST_F(OracleInteger64, MulRcx) {
    run_triplet(seed_state(), {0x48, 0xF7, 0xE1}, 0x801u);  // CF|OF
}

TEST_F(OracleInteger64, DivRcx) {
    State s = seed_state();
    s.gpr[0] = 100;  // RAX
    s.gpr[1] = 7;    // RCX
    s.gpr[2] = 0;    // RDX
    run_triplet(s, {0x48, 0xF7, 0xF1}, 0u);
}

TEST_F(OracleInteger64, IdivRcx) {
    State s = seed_state();
    s.gpr[0] = 100;
    s.gpr[1] = 7;
    s.gpr[2] = 0;
    run_triplet(s, {0x48, 0xF7, 0xF9}, 0u);
}

TEST_F(OracleInteger64, DecRax) {
    run_triplet(seed_state(), {0x48, 0xFF, 0xC8});
}

TEST_F(OracleInteger64, AndRaxRcx) {
    run_triplet(seed_state(), {0x48, 0x21, 0xC8});
}

TEST_F(OracleInteger64, OrRaxRcx) {
    run_triplet(seed_state(), {0x48, 0x09, 0xC8});
}

TEST_F(OracleInteger64, TzcntRaxRcx) {
    // Intel SDM Vol. 2 TZCNT: CF and ZF defined; OF, SF, PF, AF undefined.
    run_triplet(seed_state(), {0xF3, 0x48, 0x0F, 0xBC, 0xC1}, 0x41u);
}

TEST_F(OracleInteger64, LzcntRaxRcx) {
    // Intel SDM Vol. 2 LZCNT: CF and ZF defined; OF, SF, PF, AF undefined.
    run_triplet(seed_state(), {0xF3, 0x48, 0x0F, 0xBD, 0xC1}, 0x41u);
}

TEST_F(OracleInteger64, Cld) {
    State s = seed_state();
    s.rflags = 0x202 | 0x400;  // DF set
    run_triplet(s, {0xFC}, vitreo::oracle::contract::kArithFlagsMask | 0x400u);
}

TEST_F(OracleInteger64, Std) {
    State s = seed_state();
    run_triplet(s, {0xFD}, vitreo::oracle::contract::kArithFlagsMask | 0x400u);
}

TEST_F(OracleInteger64, Lfence) {
    run_triplet(seed_state(), {0x0F, 0xAE, 0xE8});
}

TEST_F(OracleInteger64, Mfence) {
    run_triplet(seed_state(), {0x0F, 0xAE, 0xF0});
}

TEST_F(OracleInteger64, Sfence) {
    run_triplet(seed_state(), {0x0F, 0xAE, 0xF8});
}

TEST_F(OracleInteger64, MovsxdRaxEcx) {
    State s = seed_state();
    s.gpr[1] = (s.gpr[1] & ~0xFFFFFFFFull) | 0xFFFFFFFFull;  // ECX = 0xFFFFFFFF
    run_triplet(s, {0x48, 0x63, 0xC1});
}

TEST_F(OracleInteger64, LockXaddRdiRax) {
    run_triplet(seed_state(), {0xF0, 0x48, 0x0F, 0xC1, 0x07});
}

TEST_F(OracleInteger64, Popfq) {
    State s = seed_state();
    s.gpr[4] = vitreo::oracle::kMemVa + 0xF8;
    const uint64_t flags = 0x202ull | 0x8D5ull;
    std::memcpy(s.mem.data() + 0xF8, &flags, 8);
    CompareOpts opts;
    opts.compare_rsp = true;
    run_triplet(s, {0x9D}, opts);
}

TEST_F(OracleInteger64, RetNearImm16) {
    State s = seed_state();
    s.gpr[4] = vitreo::oracle::kMemVa + 0xF0;
    const uint64_t ret_to = vitreo::oracle::kCodeVa64 + 3;  // C2 08 00
    std::memcpy(s.mem.data() + 0xF0, &ret_to, 8);
    CompareOpts opts;
    opts.compare_rsp = true;
    run_triplet(s, {0xC2, 0x08, 0x00}, opts);
}

TEST_F(OracleInteger64, CdqPreservesRaxUpper) {
    State s = seed_state();
    s.gpr[0] = 0x0000ABCD80000001ull;
    s.gpr[2] = 0x1111111122222222ull;
    run_triplet(s, {0x99});
}

TEST_F(OracleInteger64, CmovbR32NotTakenZeroExtends) {
    State s = seed_state();
    s.gpr[0] = 0xDEADBEEFCAFEF00Dull;
    s.gpr[1] = 0x0000000012345678ull;
    s.rflags = 0x202;  // CF clear
    run_triplet(s, {0x0F, 0x42, 0xC1});
}

TEST_F(OracleInteger64, ShlEaxImm0ZeroExtends) {
    State s = seed_state();
    s.gpr[0] = 0x0000FEED12345678ull;
    run_triplet(s, {0xC1, 0xE0, 0x00}, 0x41u | 0x84u);  // CF|ZF|SF|PF
}

TEST_F(OracleInteger64, DivRaxRcxFits) {
    // Unsigned 128/64: RDX < RCX so the quotient fits in RAX (SDM DIV).
    State s = seed_state();
    s.gpr[0] = 0x0000123456789ABCull;
    s.gpr[1] = 0x0000000000000007ull;
    s.gpr[2] = 0x0000000000000003ull;
    run_triplet(s, {0x48, 0xF7, 0xF1}, 0u);
}

TEST_F(OracleInteger64, IdivRaxRcxCqo) {
    // Signed 128/64 after CQO: dividend is just RAX, divisor != 0 / -1.
    State s = seed_state();
    s.gpr[0] = 0xFFFFFFFFFFFFFFF7ull;  // -9
    s.gpr[1] = 0x0000000000000004ull;
    s.gpr[2] = ~0ull;
    run_triplet(s, {0x48, 0xF7, 0xF9}, 0u);
}

// Intel SDM Vol. 2 RCL/RCR: rotate through CF. OF is defined only when the
// masked count is 1; SF/ZF/AF/PF are unchanged. Mask OF for count != 1.
TEST_F(OracleInteger64, RclEax1) {
    State s = seed_state();
    s.gpr[0] = 0x0000FEED80000001ull;
    s.rflags = 0x202 | 0x001;  // CF set so the rotate is not a plain SHL
    run_triplet(s, {0xD1, 0xD0});
}

TEST_F(OracleInteger64, RclEaxImm3) {
    State s = seed_state();
    s.gpr[0] = 0x0000FEED80000001ull;
    s.rflags = 0x202 | 0x001;
    run_triplet(s, {0xC1, 0xD0, 0x03}, kFlagsNoOf);
}

TEST_F(OracleInteger64, RclEaxCl) {
    State s = seed_state();
    s.gpr[0] = 0x0000FEED80000001ull;
    s.gpr[1] = (s.gpr[1] & ~0xFFull) | 0x03ull;
    s.rflags = 0x202 | 0x001;
    run_triplet(s, {0xD3, 0xD0}, kFlagsNoOf);
}

TEST_F(OracleInteger64, RclRax1) {
    State s = seed_state();
    s.gpr[0] = 0x8000FEED80000001ull;
    s.rflags = 0x202 | 0x001;
    run_triplet(s, {0x48, 0xD1, 0xD0});
}

TEST_F(OracleInteger64, RclRaxImm3) {
    State s = seed_state();
    s.gpr[0] = 0x8000FEED80000001ull;
    s.rflags = 0x202 | 0x001;
    run_triplet(s, {0x48, 0xC1, 0xD0, 0x03}, kFlagsNoOf);
}

TEST_F(OracleInteger64, RclRaxCl) {
    State s = seed_state();
    s.gpr[0] = 0x8000FEED80000001ull;
    s.gpr[1] = (s.gpr[1] & ~0xFFull) | 0x03ull;
    s.rflags = 0x202 | 0x001;
    run_triplet(s, {0x48, 0xD3, 0xD0}, kFlagsNoOf);
}

TEST_F(OracleInteger64, RcrEax1) {
    State s = seed_state();
    s.gpr[0] = 0x0000FEED80000001ull;
    s.rflags = 0x202 | 0x001;
    run_triplet(s, {0xD1, 0xD8});
}

TEST_F(OracleInteger64, RcrEaxImm3) {
    State s = seed_state();
    s.gpr[0] = 0x0000FEED80000001ull;
    s.rflags = 0x202 | 0x001;
    run_triplet(s, {0xC1, 0xD8, 0x03}, kFlagsNoOf);
}

TEST_F(OracleInteger64, RcrEaxCl) {
    State s = seed_state();
    s.gpr[0] = 0x0000FEED80000001ull;
    s.gpr[1] = (s.gpr[1] & ~0xFFull) | 0x03ull;
    s.rflags = 0x202 | 0x001;
    run_triplet(s, {0xD3, 0xD8}, kFlagsNoOf);
}

TEST_F(OracleInteger64, RcrRax1) {
    State s = seed_state();
    s.gpr[0] = 0x8000FEED80000001ull;
    s.rflags = 0x202 | 0x001;
    run_triplet(s, {0x48, 0xD1, 0xD8});
}

TEST_F(OracleInteger64, RcrRaxImm3) {
    State s = seed_state();
    s.gpr[0] = 0x8000FEED80000001ull;
    s.rflags = 0x202 | 0x001;
    run_triplet(s, {0x48, 0xC1, 0xD8, 0x03}, kFlagsNoOf);
}

TEST_F(OracleInteger64, RcrRaxCl) {
    State s = seed_state();
    s.gpr[0] = 0x8000FEED80000001ull;
    s.gpr[1] = (s.gpr[1] & ~0xFFull) | 0x03ull;
    s.rflags = 0x202 | 0x001;
    run_triplet(s, {0x48, 0xD3, 0xD8}, kFlagsNoOf);
}

// PUSHFQ/POPFQ pair: RSP is live only when seeded nonzero. After the pair
// RSP is restored; compare flags (and RSP) rather than a lone PUSHFQ that
// would leave RSP diverged from the default ignore-RSP protocol.
TEST_F(OracleInteger64, PushfqPopfq) {
    State s = seed_state();
    s.gpr[4] = vitreo::oracle::kMemVa + 0x100;
    s.rflags = 0x202 | 0x8D5;
    CompareOpts opts;
    opts.compare_rsp = true;
    run_triplet(s, {0x9C, 0x9D}, opts);
}

// Intel SDM Vol. 2 PUSH/POP: a 66H operand-size override selects a 16-bit
// operand in EVERY mode, 64-bit included — RSP moves by 2 and exactly two
// bytes are transferred. Promoting these to the 8-byte default drifts RSP by
// 6 per instruction, which is how an obfuscated Spotify.dll VM ended up
// popping a qword straddling two stack slots.
TEST_F(OracleInteger64, PushAxMovesRspByTwo) {
    State s = seed_state();
    s.gpr[4] = vitreo::oracle::kMemVa + 0x100;
    CompareOpts opts;
    opts.compare_rsp = true;
    run_triplet(s, {0x66, 0x50}, opts);  // push ax
}

TEST_F(OracleInteger64, PopAxMovesRspByTwo) {
    State s = seed_state();
    s.gpr[4] = vitreo::oracle::kMemVa + 0x100;
    CompareOpts opts;
    opts.compare_rsp = true;
    run_triplet(s, {0x66, 0x58}, opts);  // pop ax
}

TEST_F(OracleInteger64, PushPopWord16MemForms) {
    State s = seed_state();
    s.gpr[0] = vitreo::oracle::kMemVa + 0x40;
    s.gpr[4] = vitreo::oracle::kMemVa + 0x100;
    CompareOpts opts;
    opts.compare_rsp = true;
    // push word [rax]; pop word [rax]
    run_triplet(s, {0x66, 0xFF, 0x30, 0x66, 0x8F, 0x00}, opts);
}

TEST_F(OracleInteger64, PushImm16MovesRspByTwo) {
    State s = seed_state();
    s.gpr[4] = vitreo::oracle::kMemVa + 0x100;
    CompareOpts opts;
    opts.compare_rsp = true;
    run_triplet(s, {0x66, 0x68, 0x34, 0x12}, opts);  // push 0x1234
}

// The obfuscator shape that actually broke: a 16-bit push followed by a
// 64-bit pop leaves RSP 6 bytes higher and loads a qword that straddles the
// two-byte slot, not the value pushed.
TEST_F(OracleInteger64, PushAxThenPopRbxLeavesRspSixHigher) {
    State s = seed_state();
    s.gpr[4] = vitreo::oracle::kMemVa + 0x100;
    CompareOpts opts;
    opts.compare_rsp = true;
    run_triplet(s, {0x66, 0x50, 0x5B}, opts);  // push ax; pop rbx
}

TEST_F(OracleInteger64, ShlRaxClCarryIsLastBitOut) {
    // SDM: CF = last bit shifted out. Count 10 (CL & 63) takes bit 54 of
    // RAX, which is 0 here. Prior CF is set so a lazy FlagOp that leaves
    // the old carry fails this check.
    State s = seed_state();
    s.gpr[0] = 0x0000BD6EE76C5BE9ull;
    s.gpr[1] = 0x000059957E2E48CAull;  // CL = 0xCA -> 10
    s.rflags = 0x202 | 0x8D5;
    run_triplet(s, {0x48, 0xD3, 0xE0}, 0x41u | 0x84u);  // CF|ZF|SF|PF
}

TEST_F(OracleInteger64, AndnRaxRcxRdx) {
    // VEX.NDS.LZ.0F38.W1 F2: ANDN RAX, RCX, RDX. AF/PF undefined.
    State s = seed_state();
    s.gpr[0] = 0x0000FFFF0000FFFFull;
    s.gpr[1] = 0x00000000FFFFFFF0ull;
    s.gpr[2] = 0x0000FFFF0000000Full;
    run_triplet(s, {0xC4, 0xE2, 0xF0, 0xF2, 0xC2}, 0x8C1u);  // CF|ZF|SF|OF
}

TEST_F(OracleInteger64, BlsiRaxRcx) {
    // VEX.LZ.0F38.W1 F3 /3 — isolate lowest set bit of RCX into RAX.
    State s = seed_state();
    s.gpr[1] = 0x00000000000000A8ull;  // lowest set bit = 8
    run_triplet(s, {0xC4, 0xE2, 0xF8, 0xF3, 0xD9}, 0x8C1u);
}

TEST_F(OracleInteger64, BlsrRaxRcx) {
    // VEX.LZ.0F38.W1 F3 /1 — reset lowest set bit of RCX into RAX.
    State s = seed_state();
    s.gpr[1] = 0x00000000000000A8ull;  // A8 & (A8-1) = A0
    run_triplet(s, {0xC4, 0xE2, 0xF8, 0xF3, 0xC9}, 0x8C1u);
}

TEST_F(OracleInteger64, MovsqRsiRdi) {
    // REX.W A5 — copy qword [RSI] → [RDI], RSI/RDI += 8 (DF clear).
    State s = seed_state();
    s.gpr[6] = vitreo::oracle::kMemVa + 0x00;
    s.gpr[7] = vitreo::oracle::kMemVa + 0x10;
    const uint64_t src = 0x1122334455667788ull;
    std::memcpy(s.mem.data(), &src, 8);
    run_triplet(s, {0x48, 0xA5});
}

TEST_F(OracleInteger64, XlatRbxAl) {
    // D7 — AL := [RBX + AL].
    State s = seed_state();
    s.gpr[0] = (s.gpr[0] & ~0xFFull) | 0x05ull;
    s.gpr[3] = vitreo::oracle::kMemVa;
    s.mem[5] = 0xAB;
    run_triplet(s, {0xD7});
}

TEST_F(OracleInteger64, BzhiRaxRcxRdx) {
    State s = seed_state();
    s.gpr[0] = 0;
    s.gpr[1] = 0x0000FFFF12345678ull;
    s.gpr[2] = 16;  // keep low 16 bits
    run_triplet(s, {0xC4, 0xE2, 0xE8, 0xF5, 0xC1}, 0x8C1u);
}

TEST_F(OracleInteger64, JmpRaxSkipsInt3) {
    // FF E0 CC: jump over the INT3. Fall-through would raise #BP.
    State s = seed_state();
    s.gpr[0] = vitreo::oracle::kCodeVa64 + 3;
    run_triplet(s, {0xFF, 0xE0, 0xCC});
}

TEST_F(OracleInteger64, CallRm64) {
    State s = seed_state();
    s.gpr[4] = vitreo::oracle::kMemVa + 0x100;
    const uint64_t target = vitreo::oracle::kCodeVa64 + 2;  // length of FF 17
    std::memcpy(s.mem.data(), &target, 8);
    CompareOpts opts;
    opts.compare_rsp = true;
    run_triplet(s, {0xFF, 0x17}, opts);
}

// Intel SDM Vol. 2 ADD r/m32, r32 (01 /r): 32-bit RMW into [rdi+disp8].
// Long-mode default operand size is 32 bits; a 16-bit or register-only
// lowering would leave the scratch dword (or RAX[63:32]) wrong.
TEST_F(OracleInteger64, AddRm32EaxDisp70) {
    run_triplet(seed_state(), {0x01, 0x47, 0x70});
}

// Intel SDM Vol. 2 INC r/m32 (FF /0): increment the dword at [rdi+0x7c].
// CF is preserved; OF/SF/ZF/AF/PF follow the 32-bit result.
TEST_F(OracleInteger64, IncRm32Disp7C) {
    run_triplet(seed_state(), {0xFF, 0x47, 0x7C});
}

// Intel SDM Vol. 2 SHL r/m32, imm8 (C1 /4 ib): shl dword ptr [rdi+8], 6.
// Count != 1 ⇒ OF and AF undefined (SDM SAL/SAR/SHL/SHR).
TEST_F(OracleInteger64, ShlRm32Imm6Mem) {
    run_triplet(seed_state(), {0xC1, 0x67, 0x08, 0x06}, 0x41u | 0x84u);
}

}  // namespace
