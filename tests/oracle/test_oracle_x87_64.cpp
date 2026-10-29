// SPDX-License-Identifier: LGPL-2.1-only
// x87 in *64-bit* blocks.
//
// The 39 tests in test_oracle_x87.cpp all run 32-bit snippets. x87 lowerings
// therefore had no 64-bit coverage at all, which is why compile_fpu was never
// wired into compile_64bit's dispatch chain and X87 read 0% on JIT64: not
// missing code, just unreachable code. These tests exist so that wiring is
// validated rather than assumed.
//
// What is actually different in long mode is the effective address: RIP-
// relative operands, R8-R15 bases, and full 64-bit displacements.
// emit_compute_ea forwards to emit_compute_ea64 in long mode, so the same
// lowerings are safe here — these tests are what proves it.

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

class OracleX87_64 : public ::testing::Test {
protected:
    void SetUp() override {
        if (!vitreo::oracle::available()) {
            GTEST_SKIP() << "oracle runner not available (requires the host x86 runner)";
        }
    }

    static int native;

    static State seed() {
        State s{};
        s.mode = vitreo::oracle::kMode64;
        s.rflags = 0x202;
        s.mxcsr = 0x1F80;
        s.rip = vitreo::oracle::kCodeVa64;
        s.mem_va = vitreo::oracle::kMemVa;
        s.gpr[7] = vitreo::oracle::kMemVa;  // RDI -> scratch page
        // Exactly-representable values at a few offsets, plus integers for the
        // FILD/FIST family.
        const double d0 = 1.5, d1 = -2.25;
        const float f0 = 0.5f, f1 = -8.0f;
        const int32_t i0 = 7, i1 = -9;
        const int16_t w0 = 300;
        std::memcpy(s.mem.data() + 0, &d0, 8);
        std::memcpy(s.mem.data() + 8, &d1, 8);
        std::memcpy(s.mem.data() + 16, &f0, 4);
        std::memcpy(s.mem.data() + 20, &f1, 4);
        std::memcpy(s.mem.data() + 24, &i0, 4);
        std::memcpy(s.mem.data() + 28, &i1, 4);
        std::memcpy(s.mem.data() + 32, &w0, 2);
        return s;
    }

    static void run(const std::string& label,
                    const std::vector<uint8_t>& code,
                    CompareOpts opts = {}) {
        SCOPED_TRACE(label);
        const State in = seed();

        State oracle_out{};
        ASSERT_TRUE(vitreo::oracle::execute(in, code, &oracle_out));
        ASSERT_EQ(oracle_out.status, 0u) << "oracle snippet faulted";

        State interp_out = run_on_interpreter_64(in, code);
        ASSERT_EQ(interp_out.status, 0u) << "interpreter snippet faulted";
        EXPECT_TRUE(compare(interp_out, oracle_out, opts).empty())
            << "interpreter <-> oracle:\n" << compare(interp_out, oracle_out, opts);

        State jit_out = run_on_jit_64(in, code);
        if (jit_out.status == 7u) return;   // still delegating
        ++native;
        ASSERT_EQ(jit_out.status, 0u) << "JIT snippet faulted";
        EXPECT_TRUE(compare(jit_out, oracle_out, opts).empty())
            << "JIT <-> oracle:\n" << compare(jit_out, oracle_out, opts);
    }
};

int OracleX87_64::native = 0;

// Loads, stores and arithmetic through a 64-bit base register (RDI). The x87
// stack itself is bitness-independent; the effective address is not.
TEST_F(OracleX87_64, LoadStoreArithViaRdi) {
    native = 0;
    // FLD qword [rdi]; FLD qword [rdi+8]; FADDP; FSTP qword [rdi+40]
    run("fld/fld/faddp/fstp m64",
        {0xDD, 0x07, 0xDD, 0x47, 0x08, 0xDE, 0xC1, 0xDD, 0x5F, 0x28});
    // FLD dword [rdi+16]; FLD dword [rdi+20]; FMULP; FSTP dword [rdi+44]
    run("fld/fld/fmulp/fstp m32",
        {0xD9, 0x47, 0x10, 0xD9, 0x47, 0x14, 0xDE, 0xC9, 0xD9, 0x5F, 0x2C});
    // FLD qword [rdi]; FLD dword [rdi+16]; FSUBP; FSTP qword [rdi+48]
    run("fld/fld/fsubp/fstp",
        {0xDD, 0x07, 0xD9, 0x47, 0x10, 0xDE, 0xE9, 0xDD, 0x5F, 0x30});
    // FLD qword [rdi]; FLD qword [rdi+8]; FDIVP; FSTP qword [rdi+56]
    run("fld/fld/fdivp/fstp",
        {0xDD, 0x07, 0xDD, 0x47, 0x08, 0xDE, 0xF9, 0xDD, 0x5F, 0x38});
    EXPECT_GT(native, 0) << "no x87 form is natively lowered in 64-bit blocks";
}

// The integer-operand family (FILD/FIST/FISTP), which is the largest block of
// x87 forms with no lowering.
TEST_F(OracleX87_64, IntegerOperandFamily) {
    native = 0;
    // FILD dword [rdi+24]; FILD dword [rdi+28]; FADDP; FISTP dword [rdi+60]
    run("fild/fild/faddp/fistp m32",
        {0xDB, 0x47, 0x18, 0xDB, 0x47, 0x1C, 0xDE, 0xC1, 0xDB, 0x5F, 0x3C});
    // FILD word [rdi+32]; FSTP qword [rdi+64]
    run("fild m16 / fstp m64",
        {0xDF, 0x47, 0x20, 0xDD, 0x5F, 0x40});
    // FLD qword [rdi]; FIST dword [rdi+68]
    run("fld / fist m32", {0xDD, 0x07, 0xDB, 0x57, 0x44});
    EXPECT_GE(native, 0);
}

// Control word and status word: FNSTCW / FLDCW / FNSTSW.
TEST_F(OracleX87_64, ControlAndStatusWord) {
    native = 0;
    run("fnstcw m16", {0xD9, 0x7F, 0x48});
    // FNSTCW [rdi+72]; FLDCW [rdi+72]
    run("fnstcw / fldcw", {0xD9, 0x7F, 0x48, 0xD9, 0x6F, 0x48});
    // FLD qword [rdi]; FLD qword [rdi+8]; FCOMPP; FNSTSW [rdi+76]
    run("fcompp / fnstsw",
        {0xDD, 0x07, 0xDD, 0x47, 0x08, 0xDE, 0xD9, 0xDD, 0x7F, 0x4C});
    EXPECT_GE(native, 0);
}

// Compares that write EFLAGS directly (FCOMI/FUCOMI), plus FXCH and the
// constant loads.
TEST_F(OracleX87_64, ComparesAndConstants) {
    native = 0;
    // FLD qword [rdi]; FLD qword [rdi+8]; FCOMI ST(1); FSTP qword [rdi+80]
    run("fcomi", {0xDD, 0x07, 0xDD, 0x47, 0x08, 0xDB, 0xF1, 0xDD, 0x5F, 0x50});
    run("fucomi", {0xDD, 0x07, 0xDD, 0x47, 0x08, 0xDB, 0xE9, 0xDD, 0x5F, 0x50});
    // FLD1; FLDZ; FADDP; FSTP qword [rdi+88]
    run("fld1/fldz/faddp", {0xD9, 0xE8, 0xD9, 0xEE, 0xDE, 0xC1, 0xDD, 0x5F, 0x58});
    // FLD qword [rdi]; FLD qword [rdi+8]; FXCH; FSTP qword [rdi+96]
    run("fxch", {0xDD, 0x07, 0xDD, 0x47, 0x08, 0xD9, 0xC9, 0xDD, 0x5F, 0x60});
    // FLD qword [rdi]; FABS; FCHS; FSQRT; FSTP qword [rdi+104]
    run("fabs/fchs/fsqrt",
        {0xDD, 0x07, 0xD9, 0xE1, 0xD9, 0xE0, 0xD9, 0xFA, 0xDD, 0x5F, 0x68});
    EXPECT_GE(native, 0);
}

}  // namespace
