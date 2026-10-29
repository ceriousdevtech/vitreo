// SPDX-License-Identifier: LGPL-2.1-only
// x86-64 interpreter/JIT <-> Oracle tests for BMI1 and BMI2.
//
// These are CPUID-advertised (CPUID.7.0:EBX bits 3 and 8) but had almost no
// native JIT lowering, so they are written while they still delegate.
// run_on_jit_64 reproduces the interpreter exactly, which makes these a check
// on the *interpreter* against real x86. Once native lowerings exist the same
// tests check the lowering.
//
// Flag notes (Intel SDM Vol. 2):
//   ANDN/BLSI/BLSMSK/BLSR/BEXTR/BZHI write SF, ZF, CF and clear OF; AF and PF
//   are undefined, so they are masked out of the comparison.
//   MULX/RORX/SARX/SHLX/SHRX/PDEP/PEXT do not touch EFLAGS at all.

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

// AF (0x10) and PF (0x04) are documented undefined for the BMI flag writers.
constexpr uint32_t kBmiFlagsMask = kArithFlagsMask & ~0x14u;

class OracleBmi64 : public ::testing::Test {
protected:
    void SetUp() override {
        if (!vitreo::oracle::available()) {
            GTEST_SKIP() << "oracle runner not available (requires the host x86 runner)";
        }
    }

    static State seed(uint64_t rax, uint64_t rcx, uint64_t rdx) {
        State s{};
        s.mode = vitreo::oracle::kMode64;
        s.rflags = 0x202 | kArithFlagsMask;  // start with every arith flag set
        s.mxcsr = 0x1F80;
        s.rip = vitreo::oracle::kCodeVa64;
        s.mem_va = vitreo::oracle::kMemVa;
        s.gpr[0] = rax;
        s.gpr[1] = rcx;
        s.gpr[2] = rdx;
        s.gpr[7] = vitreo::oracle::kMemVa;  // RDI -> scratch page
        for (size_t b = 0; b < s.mem.size(); ++b) {
            s.mem[b] = static_cast<uint8_t>(0xA0 + b);
        }
        return s;
    }

    static void run_triplet(const State& in, const std::vector<uint8_t>& code,
                            uint32_t flags_mask = kBmiFlagsMask) {
        CompareOpts opts{};
        opts.flags_mask = flags_mask;

        State oracle_out{};
        ASSERT_TRUE(vitreo::oracle::execute(in, code, &oracle_out))
            << "oracle execution failed";
        if (oracle_out.status == 2u) {
            GTEST_SKIP() << "Rosetta does not implement this instruction";
        }
        ASSERT_EQ(oracle_out.status, 0u) << "oracle snippet faulted";

        State interp_out = run_on_interpreter_64(in, code);
        ASSERT_EQ(interp_out.status, 0u) << "interpreter snippet faulted";
        EXPECT_TRUE(compare(interp_out, oracle_out, opts).empty())
            << "interpreter <-> oracle divergence:\n"
            << compare(interp_out, oracle_out, opts);

        State jit_out = run_on_jit_64(in, code);
        if (jit_out.status == 7u) {
            GTEST_SKIP() << "JIT has no native lowering for this snippet";
        }
        ASSERT_EQ(jit_out.status, 0u) << "JIT snippet faulted";
        EXPECT_TRUE(compare(jit_out, oracle_out, opts).empty())
            << "JIT <-> oracle divergence:\n"
            << compare(jit_out, oracle_out, opts);
    }

    // Values chosen to hit the edges each instruction actually cares about:
    // zero, a single low bit, a single high bit, all-ones, and a mixed pattern.
    static const std::vector<uint64_t>& probes() {
        static const std::vector<uint64_t> v{
            0x0000'0000'0000'0000ull,
            0x0000'0000'0000'0001ull,
            0x0000'0000'8000'0000ull,
            0x0000'0000'FFFF'FFFFull,
            0xFFFF'FFFF'FFFF'FFFFull,
            0x0000'0000'0000'00F0ull,
            0x1234'5678'9ABC'DEF0ull,
            0x8000'0000'0000'0000ull,
        };
        return v;
    }
};

// ---------------------------------------------------------------------------
// BMI1
// ---------------------------------------------------------------------------

// ANDN EAX, ECX, EDX — C4 E2 70 F2 C2 (dest=EAX, src1=ECX via vvvv, src2=EDX)
TEST_F(OracleBmi64, Andn32) {
    for (uint64_t a : probes()) {
        for (uint64_t b : probes()) {
            run_triplet(seed(0xDEAD'BEEF'DEAD'BEEFull, a, b),
                        {0xC4, 0xE2, 0x70, 0xF2, 0xC2});
        }
    }
}

// ANDN RAX, RCX, RDX — C4 E2 F0 F2 C2 (VEX.W1)
TEST_F(OracleBmi64, Andn64) {
    for (uint64_t a : probes()) {
        for (uint64_t b : probes()) {
            run_triplet(seed(0xDEAD'BEEF'DEAD'BEEFull, a, b),
                        {0xC4, 0xE2, 0xF0, 0xF2, 0xC2});
        }
    }
}

// BLSR EAX, ECX — C4 E2 78 F3 C9 (dest in vvvv, /1)
TEST_F(OracleBmi64, Blsr32) {
    for (uint64_t v : probes()) {
        run_triplet(seed(0, v, 0), {0xC4, 0xE2, 0x78, 0xF3, 0xC9});
    }
}

TEST_F(OracleBmi64, Blsr64) {
    for (uint64_t v : probes()) {
        run_triplet(seed(0, v, 0), {0xC4, 0xE2, 0xF8, 0xF3, 0xC9});
    }
}

// BLSMSK EAX, ECX — /2
TEST_F(OracleBmi64, Blsmsk32) {
    for (uint64_t v : probes()) {
        run_triplet(seed(0, v, 0), {0xC4, 0xE2, 0x78, 0xF3, 0xD1});
    }
}

TEST_F(OracleBmi64, Blsmsk64) {
    for (uint64_t v : probes()) {
        run_triplet(seed(0, v, 0), {0xC4, 0xE2, 0xF8, 0xF3, 0xD1});
    }
}

// BLSI EAX, ECX — /3
TEST_F(OracleBmi64, Blsi32) {
    for (uint64_t v : probes()) {
        run_triplet(seed(0, v, 0), {0xC4, 0xE2, 0x78, 0xF3, 0xD9});
    }
}

TEST_F(OracleBmi64, Blsi64) {
    for (uint64_t v : probes()) {
        run_triplet(seed(0, v, 0), {0xC4, 0xE2, 0xF8, 0xF3, 0xD9});
    }
}

// Intel SDM Vol. 2 BEXTR: "ZF is updated based on the result. AF, SF, and PF
// are undefined. CF and OF are cleared." SF is undefined here but defined for
// the other BMI1 writers, so it needs its own mask.
constexpr uint32_t kBextrFlagsMask = kBmiFlagsMask & ~0x80u;

// BEXTR EAX, ECX, EDX — C4 E2 68 F7 C1. EDX[7:0] = start, EDX[15:8] = length.
TEST_F(OracleBmi64, Bextr32) {
    static const uint64_t controls[] = {
        0x0000, 0x0800, 0x0801, 0x1008, 0x2000, 0x0020, 0xFFFF, 0x4010,
    };
    for (uint64_t v : probes()) {
        for (uint64_t c : controls) {
            run_triplet(seed(0, v, c), {0xC4, 0xE2, 0x68, 0xF7, 0xC1},
                        kBextrFlagsMask);
        }
    }
}

TEST_F(OracleBmi64, Bextr64) {
    static const uint64_t controls[] = {
        0x0000, 0x0800, 0x2010, 0x4000, 0x0040, 0xFFFF, 0x203F,
    };
    for (uint64_t v : probes()) {
        for (uint64_t c : controls) {
            run_triplet(seed(0, v, c), {0xC4, 0xE2, 0xE8, 0xF7, 0xC1},
                        kBextrFlagsMask);
        }
    }
}

// ---------------------------------------------------------------------------
// BMI2
// ---------------------------------------------------------------------------

// BZHI EAX, ECX, EDX — C4 E2 68 F5 C1. EDX[7:0] is the bit index.
TEST_F(OracleBmi64, Bzhi32) {
    static const uint64_t indices[] = {0, 1, 8, 31, 32, 33, 63, 64, 0xFF, 0x100};
    for (uint64_t v : probes()) {
        for (uint64_t i : indices) {
            run_triplet(seed(0, v, i), {0xC4, 0xE2, 0x68, 0xF5, 0xC1});
        }
    }
}

TEST_F(OracleBmi64, Bzhi64) {
    static const uint64_t indices[] = {0, 1, 31, 32, 63, 64, 65, 0xFF, 0x100};
    for (uint64_t v : probes()) {
        for (uint64_t i : indices) {
            run_triplet(seed(0, v, i), {0xC4, 0xE2, 0xE8, 0xF5, 0xC1});
        }
    }
}

// MULX EAX, ECX, EDX — C4 E2 73 F6 C2. Implicit EDX multiplicand; high half to
// the ModRM.reg dest, low half to vvvv. No flags.
TEST_F(OracleBmi64, Mulx32) {
    for (uint64_t a : probes()) {
        for (uint64_t b : probes()) {
            run_triplet(seed(a, 0, b), {0xC4, 0xE2, 0x73, 0xF6, 0xC2});
        }
    }
}

TEST_F(OracleBmi64, Mulx64) {
    for (uint64_t a : probes()) {
        for (uint64_t b : probes()) {
            run_triplet(seed(a, 0, b), {0xC4, 0xE2, 0xF3, 0xF6, 0xC2});
        }
    }
}

// RORX EAX, ECX, imm8 — C4 E3 7B F0 C1 ib. No flags; count masks to 5/6 bits.
TEST_F(OracleBmi64, Rorx32) {
    static const uint8_t counts[] = {0, 1, 7, 31, 32, 33, 63, 255};
    for (uint64_t v : probes()) {
        for (uint8_t c : counts) {
            run_triplet(seed(0, v, 0), {0xC4, 0xE3, 0x7B, 0xF0, 0xC1, c});
        }
    }
}

TEST_F(OracleBmi64, Rorx64) {
    static const uint8_t counts[] = {0, 1, 7, 31, 32, 63, 64, 255};
    for (uint64_t v : probes()) {
        for (uint8_t c : counts) {
            run_triplet(seed(0, v, 0), {0xC4, 0xE3, 0xFB, 0xF0, 0xC1, c});
        }
    }
}

// SHLX / SHRX / SARX EAX, ECX, EDX. Count in EDX, masked to 5 or 6 bits.
// Unlike SHL/SHR/SAR these leave EFLAGS completely alone, including for a
// zero count -- the case that caught the plain shifts earlier in this program.
TEST_F(OracleBmi64, Shlx32) {
    static const uint64_t counts[] = {0, 1, 31, 32, 33, 63, 64, 0xFF};
    for (uint64_t v : probes()) {
        for (uint64_t c : counts) {
            run_triplet(seed(0, v, c), {0xC4, 0xE2, 0x69, 0xF7, 0xC1});
        }
    }
}

TEST_F(OracleBmi64, Shrx32) {
    // SHRX is pp=F2 (byte2 ...11), SARX is pp=F3 (byte2 ...10). The decoder
    // had these two swapped; real silicon settled it.
    static const uint64_t counts[] = {0, 1, 31, 32, 33, 63, 64, 0xFF};
    for (uint64_t v : probes()) {
        for (uint64_t c : counts) {
            run_triplet(seed(0, v, c), {0xC4, 0xE2, 0x6B, 0xF7, 0xC1});
        }
    }
}

TEST_F(OracleBmi64, Sarx32) {
    static const uint64_t counts[] = {0, 1, 31, 32, 33, 63, 64, 0xFF};
    for (uint64_t v : probes()) {
        for (uint64_t c : counts) {
            run_triplet(seed(0, v, c), {0xC4, 0xE2, 0x6A, 0xF7, 0xC1});
        }
    }
}

TEST_F(OracleBmi64, Shlx64) {
    static const uint64_t counts[] = {0, 1, 31, 32, 63, 64, 65, 0xFF};
    for (uint64_t v : probes()) {
        for (uint64_t c : counts) {
            run_triplet(seed(0, v, c), {0xC4, 0xE2, 0xE9, 0xF7, 0xC1});
        }
    }
}

TEST_F(OracleBmi64, Shrx64) {
    static const uint64_t counts[] = {0, 1, 31, 32, 63, 64, 65, 0xFF};
    for (uint64_t v : probes()) {
        for (uint64_t c : counts) {
            run_triplet(seed(0, v, c), {0xC4, 0xE2, 0xEB, 0xF7, 0xC1});
        }
    }
}

TEST_F(OracleBmi64, Sarx64) {
    static const uint64_t counts[] = {0, 1, 31, 32, 63, 64, 65, 0xFF};
    for (uint64_t v : probes()) {
        for (uint64_t c : counts) {
            run_triplet(seed(0, v, c), {0xC4, 0xE2, 0xEA, 0xF7, 0xC1});
        }
    }
}

// PEXT / PDEP EAX, ECX, EDX — mask in EDX. No flags. These have no ARM64
// equivalent, so a helper call is the correct lowering rather than an
// open-coded loop.
TEST_F(OracleBmi64, Pext32) {
    for (uint64_t v : probes()) {
        for (uint64_t m : probes()) {
            run_triplet(seed(0, v, m), {0xC4, 0xE2, 0x72, 0xF5, 0xC2});
        }
    }
}

TEST_F(OracleBmi64, Pdep32) {
    for (uint64_t v : probes()) {
        for (uint64_t m : probes()) {
            run_triplet(seed(0, v, m), {0xC4, 0xE2, 0x73, 0xF5, 0xC2});
        }
    }
}

TEST_F(OracleBmi64, Pext64) {
    for (uint64_t v : probes()) {
        for (uint64_t m : probes()) {
            run_triplet(seed(0, v, m), {0xC4, 0xE2, 0xF2, 0xF5, 0xC2});
        }
    }
}

TEST_F(OracleBmi64, Pdep64) {
    for (uint64_t v : probes()) {
        for (uint64_t m : probes()) {
            run_triplet(seed(0, v, m), {0xC4, 0xE2, 0xF3, 0xF5, 0xC2});
        }
    }
}

// ---------------------------------------------------------------------------
// MOVBE — byte-swapped load/store, no flags. Memory operand only (the
// register-source form is #UD).
// ---------------------------------------------------------------------------

// MOVBE EAX, [RDI] — 0F 38 F0 07
TEST_F(OracleBmi64, MovbeLoad32) {
    run_triplet(seed(0xDEAD'BEEF'DEAD'BEEFull, 0, 0),
                {0x0F, 0x38, 0xF0, 0x07}, kArithFlagsMask);
}

// MOVBE RAX, [RDI] — REX.W 0F 38 F0 07
TEST_F(OracleBmi64, MovbeLoad64) {
    run_triplet(seed(0xDEAD'BEEF'DEAD'BEEFull, 0, 0),
                {0x48, 0x0F, 0x38, 0xF0, 0x07}, kArithFlagsMask);
}

// MOVBE AX, [RDI] — 66 0F 38 F0 07. Only the low 16 bits of RAX are written.
TEST_F(OracleBmi64, MovbeLoad16) {
    run_triplet(seed(0xDEAD'BEEF'DEAD'BEEFull, 0, 0),
                {0x66, 0x0F, 0x38, 0xF0, 0x07}, kArithFlagsMask);
}

// MOVBE [RDI], EAX — 0F 38 F1 07
TEST_F(OracleBmi64, MovbeStore32) {
    run_triplet(seed(0x0123'4567'89AB'CDEFull, 0, 0),
                {0x0F, 0x38, 0xF1, 0x07}, kArithFlagsMask);
}

TEST_F(OracleBmi64, MovbeStore64) {
    run_triplet(seed(0x0123'4567'89AB'CDEFull, 0, 0),
                {0x48, 0x0F, 0x38, 0xF1, 0x07}, kArithFlagsMask);
}

// MOVBE [RDI], AX — writes exactly two bytes; the rest of the page must not move.
TEST_F(OracleBmi64, MovbeStore16) {
    run_triplet(seed(0x0123'4567'89AB'CDEFull, 0, 0),
                {0x66, 0x0F, 0x38, 0xF1, 0x07}, kArithFlagsMask);
}

// ---------------------------------------------------------------------------
// F16C. ARM64 FCVTL/FCVTN are the same conversions, but the two architectures
// reach them through different control registers (MXCSR vs FPCR), so the
// interesting cases are the ones where that could show: denormals, NaN payload
// and sign, infinities, and values that overflow the half range.
// ---------------------------------------------------------------------------

static State f16_seed(const uint32_t lanes[4]) {
    State s{};
    s.mode = vitreo::oracle::kMode64;
    s.rflags = 0x202;
    s.mxcsr = 0x1F80;
    s.rip = vitreo::oracle::kCodeVa64;
    s.mem_va = vitreo::oracle::kMemVa;
    s.gpr[7] = vitreo::oracle::kMemVa;
    for (int i = 0; i < 4; ++i) {
        std::memcpy(s.xmm[1].data() + i * 4, &lanes[i], 4);
    }
    return s;
}

// VCVTPH2PS xmm0, xmm1 — C4 E2 79 13 C1
TEST_F(OracleBmi64, Vcvtph2psWidens) {
    static const uint32_t cases[][4] = {
        {0x3C00BC00u, 0x00008000u, 0u, 0u},  // halves: 1, -1, +0, -0
        {0x7C00FC00u, 0x7E003C01u, 0u, 0u},  // inf, -inf, NaN, 1+eps
        {0x03FF0001u, 0x7BFF0400u, 0u, 0u},  // max/min subnormal, max normal
    };
    for (const auto& c : cases) {
        run_triplet(f16_seed(c), {0xC4, 0xE2, 0x79, 0x13, 0xC1},
                    kArithFlagsMask);
    }
}

// The 256-bit forms exercise FCVTL2 / FCVTN2 (the upper-lane halves).
TEST_F(OracleBmi64, Vcvtph2psYmm) {
    static const uint32_t c[4] = {0x3C00BC00u, 0x7C00FC00u, 0x03FF0001u, 0x7BFF0400u};
    run_triplet(f16_seed(c), {0xC4, 0xE2, 0x7D, 0x13, 0xC1}, kArithFlagsMask);
}

}  // namespace
