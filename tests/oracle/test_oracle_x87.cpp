// SPDX-License-Identifier: LGPL-2.1-only
// Interpreter ↔ Oracle x87/MMX contract tests. x87 FLD* has no JIT stencil;
// snippets that contain D8-DF or MM registers use run_pair (interpreter vs
// Rosetta), not run_triplet.

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

class OracleX87 : public ::testing::Test {
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

    static State sse_state() {
        State s{};
        s.gpr[7] = vitreo::oracle::kMemVa;  // EDI → scratch page
        s.mxcsr = 0x1F80;
        s.rflags = 0x202;
        return s;
    }

    static void put_u16(State& state, size_t offset, uint16_t value) {
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

// FLD* FXSAVE: compare ST0 (bytes 32-41); skip FOP/FIP/FDP, empty ST leftovers
// (42-159), and the 32-bit FXSAVE XMM8-15/reserved tail.
static CompareOpts fld_st0_fxsave_opts() {
    CompareOpts opts;
    opts.mem_skip_off = vitreo::oracle::contract::kFxsavePtrOff;
    opts.mem_skip_len = vitreo::oracle::contract::kFxsavePtrLen;
    opts.mem_skip2_off = 42;
    opts.mem_skip2_len = 118;
    opts.mem_skip3_off = vitreo::oracle::contract::kFxsaveLegacyRsvdOff;
    opts.mem_skip3_len = vitreo::oracle::contract::kFxsaveLegacyRsvdLen;
    return opts;
}

TEST_F(OracleX87, FldzThenFxsave) {
    // FLDZ; FXSAVE [EDI]. ST0 = +0.0 (80-bit).
    State in = sse_state();
    run_pair(in, {0xD9, 0xEE, 0x0F, 0xAE, 0x07}, fld_st0_fxsave_opts());
}

TEST_F(OracleX87, FchsThenFxsave) {
    // FLD1; FCHS; FXSAVE [EDI]. ST0 = -1.0.
    State in = sse_state();
    run_pair(in, {0xD9, 0xE8, 0xD9, 0xE0, 0x0F, 0xAE, 0x07}, fld_st0_fxsave_opts());
}

TEST_F(OracleX87, FaddSt0St0) {
    // FLD1; FADD ST(0),ST(0); FXSAVE [EDI]. ST0 = 2.0.
    State in = sse_state();
    run_pair(in, {0xD9, 0xE8, 0xDC, 0xC0, 0x0F, 0xAE, 0x07}, fld_st0_fxsave_opts());
}

TEST_F(OracleX87, FldcwThenFstcw) {
    // FLDCW [EDI]; FNSTCW [EDI+2]. Control word 0x037F round-trips.
    // FLDCW has no JIT stencil.
    State in = sse_state();
    put_u16(in, 0, 0x037F);
    run_pair(in, {0xD9, 0x2F, 0xD9, 0x7F, 0x02});
}

TEST_F(OracleX87, MmxPxorThenMovq) {
    // MOVQ mm0, [EDI]; PXOR mm0, mm0; MOVQ [EDI+8], mm0.
    // PADDB mm is not in the stencil list; MOVQ mm may not be jitable.
    State in = sse_state();
    const uint8_t src[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    std::memcpy(in.mem.data(), src, sizeof(src));
    // MMX writes set FTW to all-valid and stamp the exponent field; Rosetta
    // models neither. Verified against real silicon (Windows, 2026-08-30):
    // hardware reports ftw=0xff and Vitreo matches it exactly, Rosetta says
    // 0x00. See CompareOpts::skip_x87_tags_and_regs. The stored result is
    // still compared through memory.
    CompareOpts opts;
    opts.skip_x87_tags_and_regs = true;
    run_pair(in, {0x0F, 0x6F, 0x07, 0x0F, 0xEF, 0xC0, 0x0F, 0x7F, 0x47, 0x08},
             opts);
}

TEST_F(OracleX87, FldpiThenFxsave) {
    // FLDPI; FXSAVE [EDI]. ST0 = 80-bit π (not binary64-rounded).
    State in = sse_state();
    run_pair(in, {0xD9, 0xEB, 0x0F, 0xAE, 0x07}, fld_st0_fxsave_opts());
}

TEST_F(OracleX87, Fldl2tThenFxsave) {
    State in = sse_state();
    run_pair(in, {0xD9, 0xE9, 0x0F, 0xAE, 0x07}, fld_st0_fxsave_opts());
}

TEST_F(OracleX87, Fldl2eThenFxsave) {
    State in = sse_state();
    run_pair(in, {0xD9, 0xEA, 0x0F, 0xAE, 0x07}, fld_st0_fxsave_opts());
}

TEST_F(OracleX87, Fldlg2ThenFxsave) {
    State in = sse_state();
    run_pair(in, {0xD9, 0xEC, 0x0F, 0xAE, 0x07}, fld_st0_fxsave_opts());
}

TEST_F(OracleX87, Fldln2ThenFxsave) {
    State in = sse_state();
    run_pair(in, {0xD9, 0xED, 0x0F, 0xAE, 0x07}, fld_st0_fxsave_opts());
}

TEST_F(OracleX87, FabsThenFxsave) {
    // FLD1; FCHS; FABS; FXSAVE [EDI]. ST0 = +1.0.
    State in = sse_state();
    run_pair(in, {0xD9, 0xE8, 0xD9, 0xE0, 0xD9, 0xE1, 0x0F, 0xAE, 0x07},
             fld_st0_fxsave_opts());
}

TEST_F(OracleX87, FxchThenFxsave) {
    // FLD1; FLDZ; FXCH ST(1); FXSAVE [EDI]. ST0=1.0, ST1=+0.0.
    // Skip ST2-ST7 leftovers only (64-159).
    State in = sse_state();
    CompareOpts opts;
    opts.mem_skip_off = vitreo::oracle::contract::kFxsavePtrOff;
    opts.mem_skip_len = vitreo::oracle::contract::kFxsavePtrLen;
    opts.mem_skip2_off = 64;
    opts.mem_skip2_len = 96;
    opts.mem_skip3_off = vitreo::oracle::contract::kFxsaveLegacyRsvdOff;
    opts.mem_skip3_len = vitreo::oracle::contract::kFxsaveLegacyRsvdLen;
    run_pair(in, {0xD9, 0xE8, 0xD9, 0xEE, 0xD9, 0xC9, 0x0F, 0xAE, 0x07}, opts);
}

static CompareOpts fninit_fxsave_opts() {
    CompareOpts opts;
    opts.mem_skip_off = vitreo::oracle::contract::kFxsavePtrOff;
    opts.mem_skip_len = vitreo::oracle::contract::kFxsavePtrLen;
    opts.mem_skip2_off = vitreo::oracle::contract::kFxsaveStOff;
    opts.mem_skip2_len = vitreo::oracle::contract::kFxsaveStLen;
    opts.mem_skip3_off = vitreo::oracle::contract::kFxsaveLegacyRsvdOff;
    opts.mem_skip3_len = vitreo::oracle::contract::kFxsaveLegacyRsvdLen;
    return opts;
}

TEST_F(OracleX87, FninitThenFxsave) {
    // FNINIT; FXSAVE [EDI]. FCW/FSW/FTW match; ST leftovers and FOP/FIP/FDP
    // are not a contract (silicon does not zero them).
    State in = sse_state();
    run_pair(in, {0xDB, 0xE3, 0x0F, 0xAE, 0x07}, fninit_fxsave_opts());
}

TEST_F(OracleX87, FldStiThenFxsave) {
    // FLD1; FLD ST(0); FXSAVE [EDI]. ST0=ST1=+1.0.
    State in = sse_state();
    CompareOpts opts;
    opts.mem_skip_off = vitreo::oracle::contract::kFxsavePtrOff;
    opts.mem_skip_len = vitreo::oracle::contract::kFxsavePtrLen;
    opts.mem_skip2_off = 64;
    opts.mem_skip2_len = 96;
    opts.mem_skip3_off = vitreo::oracle::contract::kFxsaveLegacyRsvdOff;
    opts.mem_skip3_len = vitreo::oracle::contract::kFxsaveLegacyRsvdLen;
    run_pair(in, {0xD9, 0xE8, 0xD9, 0xC0, 0x0F, 0xAE, 0x07}, opts);
}

TEST_F(OracleX87, FaddpSt1ThenFxsave) {
    // FLD1; FLD1; FADDP ST(1); FXSAVE [EDI]. ST0 = 2.0.
    State in = sse_state();
    run_pair(in, {0xD9, 0xE8, 0xD9, 0xE8, 0xDE, 0xC1, 0x0F, 0xAE, 0x07},
             fld_st0_fxsave_opts());
}

TEST_F(OracleX87, FldM64ThenFxsave) {
    // FLD qword [EDI]; FXSAVE [EDI]. 2.0 binary64 → 80-bit ST0.
    State in = sse_state();
    const double two = 2.0;
    std::memcpy(in.mem.data(), &two, sizeof(two));
    run_pair(in, {0xDD, 0x07, 0x0F, 0xAE, 0x07}, fld_st0_fxsave_opts());
}

TEST_F(OracleX87, FildM32ThenFistpM32) {
    // FILD dword [EDI]; FISTP dword [EDI+4]. 42 round-trips as int32.
    State in = sse_state();
    const int32_t src = 42;
    std::memcpy(in.mem.data(), &src, sizeof(src));
    run_pair(in, {0xDB, 0x07, 0xDB, 0x5F, 0x04});
}

TEST_F(OracleX87, FtstThenFxsave) {
    // FLD1; FTST; FXSAVE [EDI]. ST0 = +1.0; C3:C2:C0 = 000 (greater).
    State in = sse_state();
    run_pair(in, {0xD9, 0xE8, 0xD9, 0xE4, 0x0F, 0xAE, 0x07},
             fld_st0_fxsave_opts());
}

TEST_F(OracleX87, FaddM64ThenFxsave) {
    // FLD1; FADD qword [EDI]; FXSAVE [EDI]. 1.0 + 2.0 = 3.0.
    State in = sse_state();
    const double two = 2.0;
    std::memcpy(in.mem.data(), &two, sizeof(two));
    run_pair(in, {0xD9, 0xE8, 0xDC, 0x07, 0x0F, 0xAE, 0x07},
             fld_st0_fxsave_opts());
}

TEST_F(OracleX87, FcomiSt1SetsEflags) {
    // FLD1; FLDZ; FCOMI ST(1). ST0=0 < ST1=1 → CF=1, ZF=0, PF=0; OF/SF/AF=0.
    State in = sse_state();
    in.rflags = 0x202 | 0x8D5u;  // all arith flags set; FCOMI must clear OF/SF/AF
    CompareOpts opts;
    opts.mem_skip_off = vitreo::oracle::contract::kFxsavePtrOff;
    opts.mem_skip_len = vitreo::oracle::contract::kFxsavePtrLen;
    opts.mem_skip2_off = 64;
    opts.mem_skip2_len = 96;
    opts.mem_skip3_off = vitreo::oracle::contract::kFxsaveLegacyRsvdOff;
    opts.mem_skip3_len = vitreo::oracle::contract::kFxsaveLegacyRsvdLen;
    run_pair(in, {0xD9, 0xE8, 0xD9, 0xEE, 0xDB, 0xF1, 0x0F, 0xAE, 0x07}, opts);
}

TEST_F(OracleX87, FnstswAxAfterFtst) {
    // FLD1; FTST; FNSTSW AX. AX[C3:C2:C0] = 000 (greater).
    State in = sse_state();
    run_pair(in, {0xD9, 0xE8, 0xD9, 0xE4, 0xDF, 0xE0});
}

TEST_F(OracleX87, FsqrtThenFxsave) {
    // FLD1; FSQRT; FXSAVE [EDI]. ST0 = +1.0.
    State in = sse_state();
    run_pair(in, {0xD9, 0xE8, 0xD9, 0xFA, 0x0F, 0xAE, 0x07},
             fld_st0_fxsave_opts());
}

TEST_F(OracleX87, FsubpSt1ThenFxsave) {
    // FLD1; FLD1; FSUBP ST(1); FXSAVE [EDI]. ST0 = 0.0.
    State in = sse_state();
    run_pair(in, {0xD9, 0xE8, 0xD9, 0xE8, 0xDE, 0xE9, 0x0F, 0xAE, 0x07},
             fld_st0_fxsave_opts());
}

TEST_F(OracleX87, FildM16ThenFistpM16) {
    // FILD word [EDI]; FISTP word [EDI+2]. 42 round-trips as int16.
    State in = sse_state();
    const int16_t src = 42;
    std::memcpy(in.mem.data(), &src, sizeof(src));
    run_pair(in, {0xDF, 0x07, 0xDF, 0x5F, 0x02});
}

TEST_F(OracleX87, FcomipSt1SetsEflagsAndPops) {
    // FLD1; FLDZ; FCOMIP ST(1). 0<1 → CF=1; pop leaves ST0=1.0.
    State in = sse_state();
    in.rflags = 0x202 | 0x8D5u;
    run_pair(in, {0xD9, 0xE8, 0xD9, 0xEE, 0xDF, 0xF1, 0x0F, 0xAE, 0x07},
             fld_st0_fxsave_opts());
}

TEST_F(OracleX87, FcmovbSt1ThenFxsave) {
    // FLD1; FLDZ; FCMOVB ST(1) with CF=1. ST0 ← ST1 = 1.0.
    State in = sse_state();
    in.rflags = 0x202 | 1u;
    CompareOpts opts;
    opts.mem_skip_off = vitreo::oracle::contract::kFxsavePtrOff;
    opts.mem_skip_len = vitreo::oracle::contract::kFxsavePtrLen;
    opts.mem_skip2_off = 64;
    opts.mem_skip2_len = 96;
    opts.mem_skip3_off = vitreo::oracle::contract::kFxsaveLegacyRsvdOff;
    opts.mem_skip3_len = vitreo::oracle::contract::kFxsaveLegacyRsvdLen;
    run_pair(in, {0xD9, 0xE8, 0xD9, 0xEE, 0xDA, 0xC1, 0x0F, 0xAE, 0x07}, opts);
}

TEST_F(OracleX87, FistM32LeavesSt0) {
    // FLD1; FIST dword [EDI]; FSTP dword [EDI+4]. Store 1; ST0 still 1 after FIST.
    State in = sse_state();
    run_pair(in, {0xD9, 0xE8, 0xDB, 0x17, 0xD9, 0x5F, 0x04});
}

TEST_F(OracleX87, FiaddM32ThenFxsave) {
    // FLD1; FIADD dword [EDI]; FXSAVE [EDI]. 1 + 2 = 3.0.
    State in = sse_state();
    const int32_t two = 2;
    std::memcpy(in.mem.data(), &two, sizeof(two));
    run_pair(in, {0xD9, 0xE8, 0xDA, 0x07, 0x0F, 0xAE, 0x07},
             fld_st0_fxsave_opts());
}

TEST_F(OracleX87, FxamThenFxsave) {
    // FLD1; FXAM; FXSAVE [EDI]. Normal +1.0: C3:C2:C0 = 010, C1=0.
    State in = sse_state();
    run_pair(in, {0xD9, 0xE8, 0xD9, 0xE5, 0x0F, 0xAE, 0x07},
             fld_st0_fxsave_opts());
}

TEST_F(OracleX87, FpremThenFxsave) {
    // FLD1; FLD1; FPREM; FXSAVE [EDI]. 1 rem 1 = 0; C2 clear.
    State in = sse_state();
    CompareOpts opts;
    opts.mem_skip_off = vitreo::oracle::contract::kFxsavePtrOff;
    opts.mem_skip_len = vitreo::oracle::contract::kFxsavePtrLen;
    opts.mem_skip2_off = 64;
    opts.mem_skip2_len = 96;
    opts.mem_skip3_off = vitreo::oracle::contract::kFxsaveLegacyRsvdOff;
    opts.mem_skip3_len = vitreo::oracle::contract::kFxsaveLegacyRsvdLen;
    run_pair(in, {0xD9, 0xE8, 0xD9, 0xE8, 0xD9, 0xF8, 0x0F, 0xAE, 0x07}, opts);
}

TEST_F(OracleX87, FaddM32ThenFxsave) {
    // FLD1; FADD dword [EDI]; FXSAVE [EDI]. 1.0 + 2.0f = 3.0.
    State in = sse_state();
    const float two = 2.0f;
    std::memcpy(in.mem.data(), &two, sizeof(two));
    run_pair(in, {0xD9, 0xE8, 0xD8, 0x07, 0x0F, 0xAE, 0x07},
             fld_st0_fxsave_opts());
}

TEST_F(OracleX87, FstSt1ThenFxsave) {
    // FLD1; FLDZ; FST ST(1); FXSAVE [EDI]. ST0=ST1=+0.0.
    State in = sse_state();
    CompareOpts opts;
    opts.mem_skip_off = vitreo::oracle::contract::kFxsavePtrOff;
    opts.mem_skip_len = vitreo::oracle::contract::kFxsavePtrLen;
    opts.mem_skip2_off = 64;
    opts.mem_skip2_len = 96;
    opts.mem_skip3_off = vitreo::oracle::contract::kFxsaveLegacyRsvdOff;
    opts.mem_skip3_len = vitreo::oracle::contract::kFxsaveLegacyRsvdLen;
    run_pair(in, {0xD9, 0xE8, 0xD9, 0xEE, 0xDD, 0xD1, 0x0F, 0xAE, 0x07}, opts);
}

TEST_F(OracleX87, FicomM32ThenFxsave) {
    // FLD1; FICOM dword [EDI]; FXSAVE [EDI]. 1 < 2 → C0=1.
    State in = sse_state();
    const int32_t two = 2;
    std::memcpy(in.mem.data(), &two, sizeof(two));
    run_pair(in, {0xD9, 0xE8, 0xDA, 0x17, 0x0F, 0xAE, 0x07},
             fld_st0_fxsave_opts());
}

TEST_F(OracleX87, FsincosZeroThenFxsave) {
    // FLDZ; FSINCOS; FXSAVE [EDI]. ST0=cos(0)=1, ST1=sin(0)=0.
    State in = sse_state();
    CompareOpts opts;
    opts.mem_skip_off = vitreo::oracle::contract::kFxsavePtrOff;
    opts.mem_skip_len = vitreo::oracle::contract::kFxsavePtrLen;
    opts.mem_skip2_off = 64;
    opts.mem_skip2_len = 96;
    opts.mem_skip3_off = vitreo::oracle::contract::kFxsaveLegacyRsvdOff;
    opts.mem_skip3_len = vitreo::oracle::contract::kFxsaveLegacyRsvdLen;
    run_pair(in, {0xD9, 0xEE, 0xD9, 0xFB, 0x0F, 0xAE, 0x07}, opts);
}

TEST_F(OracleX87, FnstenvSkipsInstructionPointers) {
    // FNSTENV [EDI]. Compare FCW/FSW/FTW only.
    //
    // Skipped: the pointer block (12..27) we don't model, and the reserved
    // upper halves of the three 16-bit-in-32-bit-slot fields (2..3, 6..7,
    // 10..11). Real Intel silicon writes 0xFFFF there -- see the FNSTENV_M28
    // case in interpreter/ops/fpu.cpp -- but the Rosetta oracle writes zero,
    // so those bytes are an oracle artefact, not a contract.
    State in = sse_state();
    CompareOpts opts;
    opts.mem_skip_off = 2;
    opts.mem_skip_len = 2;
    opts.mem_skip2_off = 6;
    opts.mem_skip2_len = 2;
    opts.mem_skip3_off = 10;
    opts.mem_skip3_len = 18;  // 10..27: reserved half + the pointer block
    run_pair(in, {0xD9, 0x37}, opts);
}

TEST_F(OracleX87, FaddToSt1ThenFxsave) {
    // FLD1; FLD1; FADD ST(1),ST(0); FXSAVE [EDI]. ST0=1, ST1=2.
    State in = sse_state();
    CompareOpts opts;
    opts.mem_skip_off = vitreo::oracle::contract::kFxsavePtrOff;
    opts.mem_skip_len = vitreo::oracle::contract::kFxsavePtrLen;
    opts.mem_skip2_off = 64;
    opts.mem_skip2_len = 96;
    opts.mem_skip3_off = vitreo::oracle::contract::kFxsaveLegacyRsvdOff;
    opts.mem_skip3_len = vitreo::oracle::contract::kFxsaveLegacyRsvdLen;
    run_pair(in, {0xD9, 0xE8, 0xD9, 0xE8, 0xDC, 0xC1, 0x0F, 0xAE, 0x07}, opts);
}

TEST_F(OracleX87, FdivrM64ThenFxsave) {
    // FLD1; FDIVR qword [EDI]; FXSAVE [EDI]. 2.0 / 1.0 = 2.0.
    State in = sse_state();
    const double two = 2.0;
    std::memcpy(in.mem.data(), &two, sizeof(two));
    run_pair(in, {0xD9, 0xE8, 0xDC, 0x3F, 0x0F, 0xAE, 0x07},
             fld_st0_fxsave_opts());
}

TEST_F(OracleX87, FnstswM16AfterFtst) {
    // FLD1; FTST; FNSTSW [EDI]. Word[C3:C2:C0] = 000 (greater).
    State in = sse_state();
    run_pair(in, {0xD9, 0xE8, 0xD9, 0xE4, 0xDD, 0x3F});
}

TEST_F(OracleX87, FldM80ThenFxsave) {
    // FLD tbyte [EDI]; FXSAVE [EDI]. 80-bit +1.0.
    State in = sse_state();
    const uint64_t mantissa = 0x8000000000000000ull;
    const uint16_t exp_sign = 0x3FFF;
    std::memcpy(in.mem.data(), &mantissa, sizeof(mantissa));
    std::memcpy(in.mem.data() + 8, &exp_sign, sizeof(exp_sign));
    run_pair(in, {0xDB, 0x2F, 0x0F, 0xAE, 0x07}, fld_st0_fxsave_opts());
}

}  // namespace
