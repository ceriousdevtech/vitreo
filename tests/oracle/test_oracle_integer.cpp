// SPDX-License-Identifier: LGPL-2.1-only
#include "oracle/oracle_client.hpp"
#include "oracle/oracle_contract.hpp"
#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {
using vitreo::oracle::State;
using vitreo::oracle::contract::run_on_interpreter;
using vitreo::oracle::contract::run_on_jit;
using vitreo::oracle::contract::compare;
using vitreo::oracle::contract::CompareOpts;

class OracleInteger32 : public ::testing::Test {
protected:
    void SetUp() override {
        if (!vitreo::oracle::available()) {
            GTEST_SKIP() << "oracle runner not available (requires the host x86 runner)";
        }
    }
    static State sse_state() {
        State s{};
        s.mxcsr = 0x1F80;
        s.gpr[7] = vitreo::oracle::kMemVa;  // EDI
        s.rflags = 0x202;
        return s;
    }
    static void run_triplet(const State& in, const std::vector<uint8_t>& code,
                            CompareOpts opts = {}) {
        State oracle_out{};
        ASSERT_TRUE(vitreo::oracle::execute(in, code, &oracle_out));
        ASSERT_EQ(oracle_out.status, 0u) << "oracle snippet faulted";
        State interp_out = run_on_interpreter(in, code);
        ASSERT_EQ(interp_out.status, 0u) << "interpreter snippet faulted";
        std::string interp_diff = compare(interp_out, oracle_out, opts);
        EXPECT_TRUE(interp_diff.empty()) << "interpreter ↔ oracle:\n" << interp_diff;
        State jit_out = run_on_jit(in, code);
        ASSERT_NE(jit_out.status, 7u) << "JIT did not compile";
        ASSERT_EQ(jit_out.status, 0u) << "JIT snippet faulted";
        std::string jit_diff = compare(jit_out, oracle_out, opts);
        EXPECT_TRUE(jit_diff.empty()) << "JIT ↔ oracle:\n" << jit_diff;
    }
    static void cmov32(uint8_t opc, uint64_t flags) {
        State in = sse_state();
        in.gpr[0] = 0x11111111u;
        in.gpr[1] = 0x22222222u;
        in.rflags = flags;
        run_triplet(in, {0x0F, opc, 0xC1});
    }
    static void setcc_al(uint8_t opc, uint64_t flags) {
        State in = sse_state();
        in.gpr[0] = 0xFFFFFF00u;
        in.rflags = flags;
        run_triplet(in, {0x0F, opc, 0xC0});
    }
};

using vitreo::oracle::contract::kArithFlagsMask;

constexpr uint64_t kFlBase = 0x202;
constexpr uint64_t kCF = 0x001;
constexpr uint64_t kPF = 0x004;
constexpr uint64_t kAF = 0x010;
constexpr uint64_t kZF = 0x040;
constexpr uint64_t kSF = 0x080;
constexpr uint64_t kDF = 0x400;
constexpr uint64_t kOF = 0x800;

// ---- CMOVcc EAX, ECX (0F 4x C1): taken and not-taken per condition ----

TEST_F(OracleInteger32, CmovoTaken) { cmov32(0x40, kFlBase | kOF); }
TEST_F(OracleInteger32, CmovoNotTaken) { cmov32(0x40, kFlBase); }
TEST_F(OracleInteger32, CmovnoTaken) { cmov32(0x41, kFlBase); }
TEST_F(OracleInteger32, CmovnoNotTaken) { cmov32(0x41, kFlBase | kOF); }
TEST_F(OracleInteger32, CmovbTaken) { cmov32(0x42, kFlBase | kCF); }
TEST_F(OracleInteger32, CmovbNotTaken) { cmov32(0x42, kFlBase); }
TEST_F(OracleInteger32, CmovnbTaken) { cmov32(0x43, kFlBase); }
TEST_F(OracleInteger32, CmovnbNotTaken) { cmov32(0x43, kFlBase | kCF); }
TEST_F(OracleInteger32, CmovnzTaken) { cmov32(0x45, kFlBase); }
TEST_F(OracleInteger32, CmovnzNotTaken) { cmov32(0x45, kFlBase | kZF); }
TEST_F(OracleInteger32, CmovbeTaken) { cmov32(0x46, kFlBase | kCF); }
TEST_F(OracleInteger32, CmovbeNotTaken) { cmov32(0x46, kFlBase); }
TEST_F(OracleInteger32, CmovnbeTaken) { cmov32(0x47, kFlBase); }
TEST_F(OracleInteger32, CmovnbeNotTaken) { cmov32(0x47, kFlBase | kCF); }
TEST_F(OracleInteger32, CmovsTaken) { cmov32(0x48, kFlBase | kSF); }
TEST_F(OracleInteger32, CmovsNotTaken) { cmov32(0x48, kFlBase); }
TEST_F(OracleInteger32, CmovnsTaken) { cmov32(0x49, kFlBase); }
TEST_F(OracleInteger32, CmovnsNotTaken) { cmov32(0x49, kFlBase | kSF); }
TEST_F(OracleInteger32, CmovpTaken) { cmov32(0x4A, kFlBase | kPF); }
TEST_F(OracleInteger32, CmovpNotTaken) { cmov32(0x4A, kFlBase); }
TEST_F(OracleInteger32, CmovnpTaken) { cmov32(0x4B, kFlBase); }
TEST_F(OracleInteger32, CmovnpNotTaken) { cmov32(0x4B, kFlBase | kPF); }
TEST_F(OracleInteger32, CmovlTaken) { cmov32(0x4C, kFlBase | kSF); }          // SF != OF
TEST_F(OracleInteger32, CmovlNotTaken) { cmov32(0x4C, kFlBase); }            // SF == OF
TEST_F(OracleInteger32, CmovnlTaken) { cmov32(0x4D, kFlBase); }              // SF == OF
TEST_F(OracleInteger32, CmovnlNotTaken) { cmov32(0x4D, kFlBase | kSF); }     // SF != OF
TEST_F(OracleInteger32, CmovleTaken) { cmov32(0x4E, kFlBase | kZF); }
TEST_F(OracleInteger32, CmovleNotTaken) { cmov32(0x4E, kFlBase); }
TEST_F(OracleInteger32, CmovnleTaken) { cmov32(0x4F, kFlBase); }             // ZF=0, SF==OF
TEST_F(OracleInteger32, CmovnleNotTaken) { cmov32(0x4F, kFlBase | kZF); }

// ---- SETcc r8 AL (0F 9x C0) ----

TEST_F(OracleInteger32, Seto) { setcc_al(0x90, kFlBase | kOF); }
TEST_F(OracleInteger32, Setno) { setcc_al(0x91, kFlBase); }
TEST_F(OracleInteger32, Setb) { setcc_al(0x92, kFlBase | kCF); }
TEST_F(OracleInteger32, Setnb) { setcc_al(0x93, kFlBase); }
TEST_F(OracleInteger32, Setnz) { setcc_al(0x95, kFlBase); }
TEST_F(OracleInteger32, Setbe) { setcc_al(0x96, kFlBase | kCF); }
TEST_F(OracleInteger32, Setnbe) { setcc_al(0x97, kFlBase); }
TEST_F(OracleInteger32, Sets) { setcc_al(0x98, kFlBase | kSF); }
TEST_F(OracleInteger32, Setns) { setcc_al(0x99, kFlBase); }
TEST_F(OracleInteger32, Setp) { setcc_al(0x9A, kFlBase | kPF); }
TEST_F(OracleInteger32, Setnp) { setcc_al(0x9B, kFlBase); }
TEST_F(OracleInteger32, Setl) { setcc_al(0x9C, kFlBase | kSF); }
TEST_F(OracleInteger32, Setnl) { setcc_al(0x9D, kFlBase); }
TEST_F(OracleInteger32, Setle) { setcc_al(0x9E, kFlBase | kZF); }
TEST_F(OracleInteger32, Setnle) { setcc_al(0x9F, kFlBase); }

// ---- Jcc / JMP rel8 that skip or fall through XOR EAX,EAX ----

TEST_F(OracleInteger32, JzTakenSkipsXor) {
    State in = sse_state();
    in.gpr[0] = 0x12345678u;
    in.rflags = kFlBase | kZF;
    run_triplet(in, {0x74, 0x02, 0x31, 0xC0});  // JZ +2; XOR EAX,EAX
}

TEST_F(OracleInteger32, JzFallthroughXorsEax) {
    State in = sse_state();
    in.gpr[0] = 0x12345678u;
    in.rflags = kFlBase;
    run_triplet(in, {0x74, 0x02, 0x31, 0xC0});
}

TEST_F(OracleInteger32, JnzTakenSkipsXor) {
    State in = sse_state();
    in.gpr[0] = 0x12345678u;
    in.rflags = kFlBase;
    run_triplet(in, {0x75, 0x02, 0x31, 0xC0});  // JNZ +2; XOR EAX,EAX
}

TEST_F(OracleInteger32, JnzFallthroughXorsEax) {
    State in = sse_state();
    in.gpr[0] = 0x12345678u;
    in.rflags = kFlBase | kZF;
    run_triplet(in, {0x75, 0x02, 0x31, 0xC0});
}

TEST_F(OracleInteger32, JmpRel8SkipsXor) {
    State in = sse_state();
    in.gpr[0] = 0x12345678u;
    run_triplet(in, {0xEB, 0x02, 0x31, 0xC0});  // JMP +2; XOR EAX,EAX
}

// ---- SHLD / SHRD. Count != 1 ⇒ OF undefined (SDM). AF also undefined. ----

TEST_F(OracleInteger32, ShldImm4) {
    State in = sse_state();
    in.gpr[0] = 0x12345678u;
    in.gpr[1] = 0x9ABCDEF0u;
    CompareOpts opts;
    opts.flags_mask = kArithFlagsMask & ~static_cast<uint32_t>(kOF | kAF);
    run_triplet(in, {0x0F, 0xA4, 0xC1, 0x04}, opts);  // SHLD r/m, r, 4
}

TEST_F(OracleInteger32, ShrdImm4) {
    State in = sse_state();
    in.gpr[0] = 0x12345678u;
    in.gpr[1] = 0x9ABCDEF0u;
    CompareOpts opts;
    opts.flags_mask = kArithFlagsMask & ~static_cast<uint32_t>(kOF | kAF);
    run_triplet(in, {0x0F, 0xAC, 0xC1, 0x04}, opts);
}

TEST_F(OracleInteger32, ShldCl) {
    State in = sse_state();
    in.gpr[0] = 0x12345678u;
    in.gpr[1] = 0x9ABCDEF4u;  // CL = 4
    CompareOpts opts;
    opts.flags_mask = kArithFlagsMask & ~static_cast<uint32_t>(kOF | kAF);
    run_triplet(in, {0x0F, 0xA5, 0xC1}, opts);
}

TEST_F(OracleInteger32, ShrdCl) {
    State in = sse_state();
    in.gpr[0] = 0x12345678u;
    in.gpr[1] = 0x9ABCDEF4u;  // CL = 4
    CompareOpts opts;
    opts.flags_mask = kArithFlagsMask & ~static_cast<uint32_t>(kOF | kAF);
    run_triplet(in, {0x0F, 0xAD, 0xC1}, opts);
}

// ---- BT / BTS / BTR / BTC. Only CF is defined (SDM). ----

TEST_F(OracleInteger32, BtsEaxEcx) {
    State in = sse_state();
    in.gpr[0] = 0xA5A5A5A5u;
    in.gpr[1] = 3;
    CompareOpts opts;
    opts.flags_mask = static_cast<uint32_t>(kCF);
    run_triplet(in, {0x0F, 0xAB, 0xC1}, opts);
}

TEST_F(OracleInteger32, BtrEaxEcx) {
    State in = sse_state();
    in.gpr[0] = 0xA5A5A5A5u;
    in.gpr[1] = 3;
    CompareOpts opts;
    opts.flags_mask = static_cast<uint32_t>(kCF);
    run_triplet(in, {0x0F, 0xB3, 0xC1}, opts);
}

TEST_F(OracleInteger32, BtcEaxEcx) {
    State in = sse_state();
    in.gpr[0] = 0xA5A5A5A5u;
    in.gpr[1] = 3;
    CompareOpts opts;
    opts.flags_mask = static_cast<uint32_t>(kCF);
    run_triplet(in, {0x0F, 0xBB, 0xC1}, opts);
}

TEST_F(OracleInteger32, BtEaxImm8) {
    State in = sse_state();
    in.gpr[0] = 0xA5A5A5A5u;
    CompareOpts opts;
    opts.flags_mask = static_cast<uint32_t>(kCF);
    run_triplet(in, {0x0F, 0xBA, 0xE0, 0x05}, opts);  // BT EAX, 5
}

// ---- XCHG ----

TEST_F(OracleInteger32, XchgEaxEcx) {
    State in = sse_state();
    in.gpr[0] = 0x11111111u;
    in.gpr[1] = 0x22222222u;
    run_triplet(in, {0x91});  // XCHG EAX, ECX
}

TEST_F(OracleInteger32, XchgEbxEcx) {
    State in = sse_state();
    in.gpr[1] = 0x22222222u;
    in.gpr[3] = 0x33333333u;
    run_triplet(in, {0x87, 0xCB});  // XCHG EBX, ECX
}

// ---- Sign extension ----

TEST_F(OracleInteger32, Cdq) {
    State in = sse_state();
    in.gpr[0] = 0x80000000u;
    run_triplet(in, {0x99});  // CDQ
}

TEST_F(OracleInteger32, Cwde) {
    State in = sse_state();
    in.gpr[0] = 0x00008000u;  // AX = 0x8000 → EAX sign-extend
    run_triplet(in, {0x98});  // CWDE
}

// ---- MUL / IMUL / DIV / IDIV ----

TEST_F(OracleInteger32, MulEcx) {
    State in = sse_state();
    in.gpr[0] = 7;
    in.gpr[1] = 9;
    CompareOpts opts;
    opts.flags_mask = 0x801;  // CF|OF only
    run_triplet(in, {0xF7, 0xE1}, opts);  // MUL ECX
}

TEST_F(OracleInteger32, ImulEcx) {
    State in = sse_state();
    in.gpr[0] = 7;
    in.gpr[1] = 9;
    CompareOpts opts;
    opts.flags_mask = 0x801;  // CF|OF; SF/ZF/AF/PF undefined
    run_triplet(in, {0xF7, 0xE9}, opts);  // IMUL r/m32 (ECX)
}

TEST_F(OracleInteger32, DivEcx) {
    State in = sse_state();
    in.gpr[0] = 100;
    in.gpr[1] = 7;
    in.gpr[2] = 0;  // EDX
    CompareOpts opts;
    opts.flags_mask = 0;  // DIV flags undefined
    run_triplet(in, {0xF7, 0xF1}, opts);  // DIV ECX
}

TEST_F(OracleInteger32, IdivEcx) {
    State in = sse_state();
    in.gpr[0] = 100;
    in.gpr[1] = 7;
    in.gpr[2] = 0;
    CompareOpts opts;
    opts.flags_mask = 0;
    run_triplet(in, {0xF7, 0xF9}, opts);  // IDIV ECX
}

TEST_F(OracleInteger32, SignedMagicDiv15) {
    // IMUL 0x88888889; ADD EDX,ECX; SAR EDX,6; sign-fix → n/15.
    const std::vector<uint8_t> code = {
        0xB8, 0x89, 0x88, 0x88, 0x88, 0xF7, 0xE9, 0x03, 0xD1,
        0xC1, 0xFA, 0x06, 0x89, 0xD0, 0xC1, 0xE8, 0x1F, 0x03, 0xC2,
    };
    const int32_t spans[] = {
        0, 15, 30, 120, 1696, 0x10000, -15, -120, 0x7FFF0000,
    };
    for (int32_t span : spans) {
        State in = sse_state();
        in.gpr[1] = static_cast<uint32_t>(span);  // ECX
        run_triplet(in, code);
    }
}

TEST_F(OracleInteger32, SignedMagicDiv48) {
    // IMUL 0x2AAAAAAB; SAR EDX,5; sign-fix → n/48.
    const std::vector<uint8_t> code = {
        0xB8, 0xAB, 0xAA, 0xAA, 0x2A, 0xF7, 0xE9, 0xC1, 0xFA, 0x05,
        0x89, 0xD0, 0xC1, 0xE8, 0x1F, 0x03, 0xC2,
    };
    const int32_t spans[] = {
        0, 48, 96, 192, 0xC000, -48, 0x7FFF0000,
    };
    for (int32_t span : spans) {
        State in = sse_state();
        in.gpr[1] = static_cast<uint32_t>(span);
        run_triplet(in, code);
    }
}

// ---- DEC / AND / OR / TEST / CMP / ADD ----

TEST_F(OracleInteger32, DecEax) {
    State in = sse_state();
    in.gpr[0] = 1;
    run_triplet(in, {0xFF, 0xC8});  // DEC EAX
}

TEST_F(OracleInteger32, AndEaxEcx) {
    State in = sse_state();
    in.gpr[0] = 0xF0F0F0F0u;
    in.gpr[1] = 0x0F0F0F0Fu;
    CompareOpts opts;
    opts.flags_mask = kArithFlagsMask & ~static_cast<uint32_t>(kAF);
    run_triplet(in, {0x21, 0xC8}, opts);  // AND EAX, ECX
}

TEST_F(OracleInteger32, OrEaxEcx) {
    State in = sse_state();
    in.gpr[0] = 0xF0F0F0F0u;
    in.gpr[1] = 0x0F0F0F0Fu;
    CompareOpts opts;
    opts.flags_mask = kArithFlagsMask & ~static_cast<uint32_t>(kAF);
    run_triplet(in, {0x09, 0xC8}, opts);  // OR EAX, ECX
}

TEST_F(OracleInteger32, TestEaxImm32) {
    State in = sse_state();
    in.gpr[0] = 0x12345678u;
    CompareOpts opts;
    opts.flags_mask = kArithFlagsMask & ~static_cast<uint32_t>(kAF);
    run_triplet(in, {0xF7, 0xC0, 0x00, 0x00, 0x00, 0xFF}, opts);
}

TEST_F(OracleInteger32, CmpAlImm8) {
    State in = sse_state();
    in.gpr[0] = 0x03;
    run_triplet(in, {0x3C, 0x05});  // CMP AL, 5
}

TEST_F(OracleInteger32, AddAlImm8) {
    State in = sse_state();
    in.gpr[0] = 0xFFu;
    run_triplet(in, {0x04, 0x01});  // ADD AL, 1
}

// ---- TZCNT / LZCNT. OF/SF/AF/PF undefined (SDM). ----

TEST_F(OracleInteger32, TzcntEaxEcx) {
    State in = sse_state();
    in.gpr[1] = 0x00F00000u;
    CompareOpts opts;
    opts.flags_mask = static_cast<uint32_t>(kCF | kZF);
    run_triplet(in, {0xF3, 0x0F, 0xBC, 0xC1}, opts);
}

TEST_F(OracleInteger32, LzcntEaxEcx) {
    State in = sse_state();
    in.gpr[1] = 0x00F00000u;
    CompareOpts opts;
    opts.flags_mask = static_cast<uint32_t>(kCF | kZF);
    run_triplet(in, {0xF3, 0x0F, 0xBD, 0xC1}, opts);
}

// ---- DF: CLD / STD ----

TEST_F(OracleInteger32, Cld) {
    State in = sse_state();
    in.rflags = kFlBase | kDF;
    CompareOpts opts;
    opts.flags_mask = static_cast<uint32_t>(kDF) | kArithFlagsMask;
    run_triplet(in, {0xFC}, opts);
}

TEST_F(OracleInteger32, Std) {
    State in = sse_state();
    CompareOpts opts;
    opts.flags_mask = static_cast<uint32_t>(kDF) | kArithFlagsMask;
    run_triplet(in, {0xFD}, opts);
}

// ---- Fences: no architectural GPR/flag change ----

TEST_F(OracleInteger32, Lfence) {
    State in = sse_state();
    run_triplet(in, {0x0F, 0xAE, 0xE8});
}

TEST_F(OracleInteger32, Mfence) {
    State in = sse_state();
    run_triplet(in, {0x0F, 0xAE, 0xF0});
}

TEST_F(OracleInteger32, Sfence) {
    State in = sse_state();
    run_triplet(in, {0x0F, 0xAE, 0xF8});
}

// ---- LOCK XADD [EDI], EAX ----

TEST_F(OracleInteger32, CmpMemImmJeElseAdd) {
    // cmp dword [esi], 6; je +3; add dword [esi], 1
    const std::vector<uint8_t> code = {
        0x83, 0x3E, 0x06, 0x74, 0x03, 0x83, 0x06, 0x01};
    for (uint32_t start : {0u, 5u, 6u, 7u}) {
        State in = sse_state();
        in.gpr[6] = vitreo::oracle::kMemVa;  // ESI
        std::memcpy(in.mem.data(), &start, 4);
        run_triplet(in, code);
    }
}

TEST_F(OracleInteger32, TestEaxEaxJneSelect) {
    // test eax, eax; jne end; mov eax, 0x11
    // Zero writes 0x11; nonzero keeps EAX. Taken Jcc lands on the snippet
    // end so one JIT execute covers both sides.
    const std::vector<uint8_t> code = {
        0x85, 0xC0, 0x75, 0x05, 0xB8, 0x11, 0x00, 0x00, 0x00};
    for (uint32_t eax : {0u, 1u, 0x80000000u, 0xFFFFFFFFu}) {
        State in = sse_state();
        in.gpr[0] = eax;
        run_triplet(in, code);
    }
}

TEST_F(OracleInteger32, TestEdiEdiJeSkipAdd) {
    // test edi, edi; je +3; add dword [esi], 1
    const std::vector<uint8_t> code = {0x85, 0xFF, 0x74, 0x03, 0x83, 0x06, 0x01};
    for (uint32_t edi : {0u, 1u, 0x80000000u}) {
        State in = sse_state();
        in.gpr[6] = vitreo::oracle::kMemVa;  // ESI
        in.gpr[7] = edi;
        uint32_t start = 5;
        std::memcpy(in.mem.data(), &start, 4);
        run_triplet(in, code);
    }
}

TEST_F(OracleInteger32, CmpMemZeroJeSelect) {
    // cmp dword [esi], 0; je end; mov eax, 0x11
    // Zero keeps EAX; nonzero writes 0x11.
    const std::vector<uint8_t> code = {
        0x83, 0x3E, 0x00, 0x74, 0x05, 0xB8, 0x11, 0x00, 0x00, 0x00};
    for (uint32_t start : {0u, 1u, 6u}) {
        State in = sse_state();
        in.gpr[0] = 0x22;
        in.gpr[6] = vitreo::oracle::kMemVa;
        std::memcpy(in.mem.data(), &start, 4);
        run_triplet(in, code);
    }
}

TEST_F(OracleInteger32, TestEaxLeaJeSelect) {
    // test eax, eax; lea edx, [ecx+edx*4]; je end; mov eax, 0x11
    // Empty-bucket check: LEA is flag-neutral (Intel SDM Vol. 2 LEA / Jcc).
    const std::vector<uint8_t> code = {
        0x85, 0xC0, 0x8D, 0x14, 0x91, 0x74, 0x05, 0xB8, 0x11, 0x00, 0x00,
        0x00};
    for (uint32_t eax : {0u, 1u, 0x80000000u}) {
        State in = sse_state();
        in.gpr[0] = eax;
        in.gpr[1] = vitreo::oracle::kMemVa;  // ECX
        in.gpr[2] = 0;                       // EDX
        run_triplet(in, code);
    }
}

TEST_F(OracleInteger32, XorEdxDivEdiRemainder) {
    // xor edx, edx; mov eax, esi; div edi
    // Unsigned hash bucket: EDX:EAX / EDI → remainder in EDX.
    const std::vector<uint8_t> code = {0x31, 0xD2, 0x89, 0xF0, 0xF7, 0xF7};
    const uint32_t cases[][2] = {
        {0xA5A5A5A5u, 8u}, {0x11u, 3u}, {0xFFFFFFFFu, 7u}};
    for (const auto& c : cases) {
        State in = sse_state();
        in.gpr[6] = c[0];
        in.gpr[7] = c[1];
        CompareOpts opts;
        opts.flags_mask = 0;  // DIV leaves EFLAGS undefined
        run_triplet(in, code, opts);
    }
}

TEST_F(OracleInteger32, CmpSibPopJeSelect) {
    // push ebx; cmp eax, [ecx+edx*4]; pop esi; je end; mov eax, 0x11
    // Same end-iterator check as a hash-bucket lookup: POP is flag-neutral
    // (Intel SDM Vol. 2 POP / Jcc). PUSH before CMP gives both sides a
    // known stack word so ESI matches. Equal keeps EAX; unequal writes 0x11.
    const std::vector<uint8_t> code = {
        0x53, 0x3B, 0x04, 0x91, 0x5E, 0x74, 0x05, 0xB8, 0x11, 0x00, 0x00,
        0x00};
    for (uint32_t slot : {0u, 0x11u, 0xFFFFFFFFu}) {
        State in = sse_state();
        in.gpr[0] = slot;
        in.gpr[1] = vitreo::oracle::kMemVa;  // ECX
        in.gpr[2] = 0;                       // EDX
        in.gpr[3] = 0xAABBCCDDu;             // EBX, pushed then popped into ESI
        in.gpr[6] = 0xDEADBEEFu;
        uint32_t end_iter = 0x11;
        std::memcpy(in.mem.data(), &end_iter, 4);
        run_triplet(in, code);
    }
}

TEST_F(OracleInteger32, CmpMemZeroJneThenStoreZero) {
    // cmp dword [esi], 0; je end; mov dword [esi+4], 0
    // Non-zero flag writes [esi+4]=0; zero flag leaves it.
    const std::vector<uint8_t> code = {
        0x83, 0x3E, 0x00, 0x74, 0x07, 0xC7, 0x46, 0x04, 0x00, 0x00, 0x00,
        0x00};
    for (uint32_t flag : {0u, 1u}) {
        State in = sse_state();
        in.gpr[6] = vitreo::oracle::kMemVa;
        uint32_t words[2] = {flag, 7};
        std::memcpy(in.mem.data(), words, sizeof(words));
        run_triplet(in, code);
    }
}

TEST_F(OracleInteger32, LockXaddMemEax) {
    State in = sse_state();
    in.gpr[0] = 2;
    in.mem[0] = 1;
    run_triplet(in, {0xF0, 0x0F, 0xC1, 0x07});
}

// ---- MOVSB ----

TEST_F(OracleInteger32, Movsb) {
    State in = sse_state();
    in.gpr[6] = vitreo::oracle::kMemVa;      // ESI
    in.gpr[7] = vitreo::oracle::kMemVa + 8;  // EDI
    in.mem[0] = 0xAB;
    in.rflags = kFlBase;  // DF clear
    run_triplet(in, {0xA4});
}

// ---- Multi-byte NOP ----

TEST_F(OracleInteger32, NopMultibyte) {
    State in = sse_state();
    run_triplet(in, {0x0F, 0x1F, 0x00});
}

// ---- 8-bit ALU ----

TEST_F(OracleInteger32, XorAlCl) {
    State in = sse_state();
    in.gpr[0] = 0xFFFFFFAAu;
    in.gpr[1] = 0x55;
    CompareOpts opts;
    opts.flags_mask = kArithFlagsMask & ~static_cast<uint32_t>(kAF);
    run_triplet(in, {0x32, 0xC1}, opts);  // XOR AL, CL
}

TEST_F(OracleInteger32, AddAlCl) {
    State in = sse_state();
    in.gpr[0] = 0xFFFFFF01u;
    in.gpr[1] = 0x01;
    run_triplet(in, {0x00, 0xC8});  // ADD AL, CL
}

// ---- 16-bit operand-size ----

TEST_F(OracleInteger32, MovAxImm16) {
    State in = sse_state();
    in.gpr[0] = 0xFFFF0000u;
    run_triplet(in, {0x66, 0xB8, 0x34, 0x12});  // MOV AX, 0x1234
}

TEST_F(OracleInteger32, AddAxCx) {
    State in = sse_state();
    in.gpr[0] = 0x11112222u;
    in.gpr[1] = 0x00003333u;
    run_triplet(in, {0x66, 0x01, 0xC8});  // ADD AX, CX
}

}  // namespace
