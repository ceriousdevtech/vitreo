// SPDX-License-Identifier: LGPL-2.1-only
// FP rounding-mode semantics: MXCSR.RC for the SSE float->int conversions, and
// x87 precision control.
//
// Both were recorded as known divergences from real Windows in an earlier
// capture. This pins the current behaviour against the oracle so the claims
// are measured rather than remembered.

#include "oracle/oracle_client.hpp"
#include "oracle/oracle_contract.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

using vitreo::oracle::State;
using vitreo::oracle::contract::compare;
using vitreo::oracle::contract::CompareOpts;
using vitreo::oracle::contract::run_on_interpreter_64;
using vitreo::oracle::contract::run_on_jit_64;

class OracleFpRounding64 : public ::testing::Test {
protected:
    void SetUp() override {
        if (!vitreo::oracle::available()) {
            GTEST_SKIP() << "oracle runner not available (requires the host x86 runner)";
        }
    }

    static State seed(uint32_t mxcsr) {
        State s{};
        s.mode = vitreo::oracle::kMode64;
        s.rflags = 0x202;
        s.mxcsr = mxcsr;
        s.rip = vitreo::oracle::kCodeVa64;
        s.mem_va = vitreo::oracle::kMemVa;
        s.gpr[7] = vitreo::oracle::kMemVa;
        return s;
    }

    static void run(const std::string& label, const State& in,
                    const std::vector<uint8_t>& code,
                    CompareOpts opts = {}) {
        SCOPED_TRACE(label);
        State oracle_out{};
        ASSERT_TRUE(vitreo::oracle::execute(in, code, &oracle_out));
        ASSERT_EQ(oracle_out.status, 0u) << "oracle snippet faulted";

        State interp_out = run_on_interpreter_64(in, code);
        ASSERT_EQ(interp_out.status, 0u);
        EXPECT_TRUE(compare(interp_out, oracle_out, opts).empty())
            << "interpreter <-> oracle:\n" << compare(interp_out, oracle_out, opts);

        State jit_out = run_on_jit_64(in, code);
        if (jit_out.status == 7u) return;
        ASSERT_EQ(jit_out.status, 0u);
        EXPECT_TRUE(compare(jit_out, oracle_out, opts).empty())
            << "JIT <-> oracle:\n" << compare(jit_out, oracle_out, opts);
    }
};

// CVTSS2SI / CVTSD2SI round according to MXCSR.RC; only the CVTT* forms
// truncate unconditionally. RC lives in MXCSR bits 14:13.
TEST_F(OracleFpRounding64, Cvtss2siHonoursMxcsrRoundingMode) {
    static const uint32_t kRc[] = {0x1F80, 0x3F80, 0x5F80, 0x7F80};  // RN,RD,RU,RZ
    static const float kVals[] = {1.5f, 2.5f, -1.5f, -2.5f, 1.4f, 1.6f,
                                  -1.4f, -1.6f, 0.5f, -0.5f};
    for (uint32_t mxcsr : kRc) {
        for (float v : kVals) {
            State in = seed(mxcsr);
            std::memcpy(in.xmm[0].data(), &v, 4);
            // CVTSS2SI eax, xmm0 — F3 0F 2D C0
            run("cvtss2si rc=" + std::to_string(mxcsr >> 13) +
                    " v=" + std::to_string(v),
                in, {0xF3, 0x0F, 0x2D, 0xC0});
        }
    }
}

TEST_F(OracleFpRounding64, Cvtsd2siHonoursMxcsrRoundingMode) {
    static const uint32_t kRc[] = {0x1F80, 0x3F80, 0x5F80, 0x7F80};
    static const double kVals[] = {1.5, 2.5, -1.5, -2.5, 1.4, 1.6, -1.4, -1.6};
    for (uint32_t mxcsr : kRc) {
        for (double v : kVals) {
            State in = seed(mxcsr);
            std::memcpy(in.xmm[0].data(), &v, 8);
            // CVTSD2SI eax, xmm0 — F2 0F 2D C0
            run("cvtsd2si rc=" + std::to_string(mxcsr >> 13), in,
                {0xF2, 0x0F, 0x2D, 0xC0});
        }
    }
}

// CVTTSS2SI truncates regardless of RC — the control case that proves a fix
// did not simply wire RC into every conversion.
TEST_F(OracleFpRounding64, CvttssTruncatesRegardlessOfRoundingMode) {
    static const uint32_t kRc[] = {0x1F80, 0x3F80, 0x5F80, 0x7F80};
    static const float kVals[] = {1.5f, -1.5f, 1.9f, -1.9f};
    for (uint32_t mxcsr : kRc) {
        for (float v : kVals) {
            State in = seed(mxcsr);
            std::memcpy(in.xmm[0].data(), &v, 4);
            // CVTTSS2SI eax, xmm0 — F3 0F 2C C0
            run("cvttss2si rc=" + std::to_string(mxcsr >> 13), in,
                {0xF3, 0x0F, 0x2C, 0xC0});
        }
    }
}

// x87 precision control: PC in control-word bits 9:8 narrows the significand
// to 24 (00) or 53 (10) bits while leaving the exponent range alone.
TEST_F(OracleFpRounding64, X87PrecisionControlNarrowsSignificand) {
    // FLDCW [rdi]; FLD qword [rdi+8]; FADD qword [rdi+16]; FSTP qword [rdi+24]
    const std::vector<uint8_t> code{
        0xD9, 0x2F,
        0xDD, 0x47, 0x08,
        0xDC, 0x47, 0x10,
        0xDD, 0x5F, 0x18,
    };
    // 0x007F = PC single(24), 0x027F = double(53), 0x037F = extended(64).
    static const uint16_t kCw[] = {0x007F, 0x027F, 0x037F};
    // A sum whose exact result needs more than 24 significand bits.
    static const double kPairs[][2] = {
        {1.0, 1.0 / 1048576.0 / 1024.0},   // 1 + 2^-30
        {1.0, 1.0 / 8388608.0},            // 1 + 2^-23 (exact at 24 bits)
        {1.0, 1.0 / 16777216.0},           // 1 + 2^-24 (ties)
        {12345.678, 0.000000123},
    };
    for (uint16_t cw : kCw) {
        for (const auto& pr : kPairs) {
            State in = seed(0x1F80);
            std::memcpy(in.mem.data() + 0, &cw, 2);
            std::memcpy(in.mem.data() + 8, &pr[0], 8);
            std::memcpy(in.mem.data() + 16, &pr[1], 8);
            // Every FSW bit participates, C1 included: the 80-bit arithmetic
            // reports the rounding direction (core/math/float80.cpp).
            CompareOpts o;
            o.fsw_mask = vitreo::oracle::contract::kFswAll;
            run("x87 PC cw=" + std::to_string(cw), in, code, o);
        }
    }
}

// x87 rounding control: RC in control-word bits 11:10 (00 nearest, 01 down,
// 10 up, 11 toward zero) selects how an inexact x87 result is rounded. The
// guest sets it with FLDCW inside the same block that then does arithmetic,
// so a backend that only records the control word without configuring the
// unit that actually rounds produces round-to-nearest for every mode.
TEST_F(OracleFpRounding64, X87RoundingControlSelectsRoundingOfInexactResult) {
    // FLDCW [rdi]; FLD qword [rdi+8]; FDIV qword [rdi+16]; FSTP qword [rdi+24]
    const std::vector<uint8_t> code{
        0xD9, 0x2F,
        0xDD, 0x47, 0x08,
        0xDC, 0x77, 0x10,
        0xDD, 0x5F, 0x18,
    };
    static const uint16_t kCw[] = {0x037F, 0x077F, 0x0B7F, 0x0F7F};
    static const double kPairs[][2] = {
        {1.0, 3.0}, {-1.0, 3.0}, {2.0, 7.0}, {-2.0, 7.0},
        {1.0, 49.0}, {10.0, 3.0},
    };
    for (uint16_t cw : kCw) {
        for (const auto& pr : kPairs) {
            State in = seed(0x1F80);
            std::memcpy(in.mem.data() + 0, &cw, 2);
            std::memcpy(in.mem.data() + 8, &pr[0], 8);
            std::memcpy(in.mem.data() + 16, &pr[1], 8);
            // Every FSW bit participates, C1 included: the 80-bit arithmetic
            // reports the rounding direction (core/math/float80.cpp).
            CompareOpts o;
            o.fsw_mask = vitreo::oracle::contract::kFswAll;
            run("x87 RC cw=" + std::to_string(cw), in, code, o);
        }
    }
}

// MXCSR.RC governs SSE *arithmetic* rounding, not only the float->int
// conversions above. DIVSD/DIVSS produce an inexact result for these operands,
// so each rounding mode gives a different low bit.
//
// FIXED 2026-08-30. This was pinned as a known divergence: Vitreo rounded SSE
// arithmetic to nearest for every MXCSR.RC, one ULP out on every inexact
// result at RC=1/2/3, because the host FPCR held whatever x87's FCW.RC last
// asked for. x87 RC and MXCSR RC are independent architectural fields and the
// host has a single FPCR, so the fix was to install the rounding mode PER
// INSTRUCTION CLASS at the point that class executes rather than once when the
// guest writes a control word -- see core/math/host_fenv.hpp.
//
// Keep this test: it is also the regression guard for the reverse direction,
// an x87 FLDCW selecting chop silently re-rounding subsequent SSE arithmetic.
TEST_F(OracleFpRounding64, SseArithmeticHonoursMxcsrRoundingMode) {
    // MOVSD xmm0,[rdi]; DIVSD xmm0,[rdi+8]; MOVSD [rdi+16],xmm0
    const std::vector<uint8_t> code_sd{
        0xF2, 0x0F, 0x10, 0x07,
        0xF2, 0x0F, 0x5E, 0x47, 0x08,
        0xF2, 0x0F, 0x11, 0x47, 0x10,
    };
    // MOVSS xmm0,[rdi]; DIVSS xmm0,[rdi+8]; MOVSS [rdi+16],xmm0
    const std::vector<uint8_t> code_ss{
        0xF3, 0x0F, 0x10, 0x07,
        0xF3, 0x0F, 0x5E, 0x47, 0x08,
        0xF3, 0x0F, 0x11, 0x47, 0x10,
    };
    static const uint32_t kRc[] = {0x1F80, 0x3F80, 0x5F80, 0x7F80};
    static const double kPairs[][2] = {
        {1.0, 3.0}, {-1.0, 3.0}, {2.0, 7.0}, {-2.0, 7.0}, {1.0, 49.0},
    };
    for (uint32_t mxcsr : kRc) {
        for (const auto& pr : kPairs) {
            State in = seed(mxcsr);
            std::memcpy(in.mem.data() + 0, &pr[0], 8);
            std::memcpy(in.mem.data() + 8, &pr[1], 8);
            run("divsd rc=" + std::to_string(mxcsr >> 13), in, code_sd);

            State in32 = seed(mxcsr);
            const float a = static_cast<float>(pr[0]);
            const float b = static_cast<float>(pr[1]);
            std::memcpy(in32.mem.data() + 0, &a, 4);
            std::memcpy(in32.mem.data() + 8, &b, 4);
            run("divss rc=" + std::to_string(mxcsr >> 13), in32, code_ss);
        }
    }
}


// Intel SDM Vol. 2 "FPU Flags Affected" for the x87 arithmetic ops: when the result is inexact, C1 (FSW bit 9) is 1 if
// the delivered value is greater than the exact one and 0 otherwise. Vitreo
// leaves C1 at 0 for FADD/FSUB/FMUL/FDIV, on the interpreter and the JIT
// alike. The integer-store family (FIST/FISTP/FISTTP) and FRNDINT DO model it
// as of this change -- those have the exact source value in hand, so it costs
// one comparison.
//
// Confirmed against real silicon on the `Windows` host, so this is a Vitreo
// gap and not a Rosetta artifact (tools/oracle_probe/x87_silicon_probe.c):
//
//   fld 1.5;  fistp m32      -> FSW=0x0220  C1 set    (now matched by Vitreo)
//   fld 1.25; fistp m32      -> FSW=0x0020  C1 clear  (now matched by Vitreo)
//   fld1; fld 2^-70; faddp   -> FSW=0x3820  C1 clear, PE set
//
// This was blocked on x87 arithmetic being evaluated in binary64, which made
// the rounding direction of the double result the wrong question. That is now
// fixed -- arithmetic runs in the architectural 80-bit format
// (core/math/float80.hpp) and core/math/float80.cpp's round_pack reports the
// direction as kF80C1, which FpuState::record_soft_status folds into FSW.
//
// FIXED as a consequence, 2026-08-30: this test passes and is now gated.
//
// Enable with --gtest_also_run_disabled_tests to see the current diff.
TEST_F(OracleFpRounding64, X87ArithmeticSetsC1WhenTheResultRoundsUp) {
    // FLD qword [rdi]; FDIV qword [rdi+8]; FSTP qword [rdi+16]
    const std::vector<uint8_t> code{
        0xDD, 0x07,
        0xDC, 0x77, 0x08,
        0xDD, 0x5F, 0x10,
    };
    static const double kPairs[][2] = {
        {1.0, 3.0}, {2.0, 3.0}, {1.0, 49.0}, {10.0, 3.0}, {-1.0, 3.0},
    };
    CompareOpts o;
    o.fsw_mask = vitreo::oracle::contract::kFswAll;
    for (const auto& pr : kPairs) {
        State in = seed(0x1F80);
        std::memcpy(in.mem.data() + 0, &pr[0], 8);
        std::memcpy(in.mem.data() + 8, &pr[1], 8);
        run("fdiv C1", in, code, o);
    }
}

}  // namespace