// SPDX-License-Identifier: LGPL-2.1-only
// Exhaustive x86-64 interpreter/JIT <-> Oracle sweep over every FMA3 form that
// has a native lowering: 4 sign variants x 3 operand orders x {PS,PD,SS,SD}.
//
// The hand-written FMA tests cover 8 of these 48. That was enough to miss a
// real operand-order bug in the lowering: the order digits were read at a fixed
// character offset into opcode_name(), which is wrong for VFNMADD/VFNMSUB
// because those mnemonics are one character longer. Only a sweep catches that
// class, so this walks the whole encoding space.
//
// x86 FMA is fused (one rounding) and so are ARM64's FMLA/FMLS and the scalar
// FMADD family, which is what makes the mapping exact. Values here are chosen
// to be exactly representable so the comparison is bit-for-bit and no MXCSR
// exception bits are raised.

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

class OracleFmaAll64 : public ::testing::Test {
protected:
    void SetUp() override {
        if (!vitreo::oracle::available()) {
            GTEST_SKIP() << "oracle runner not available (requires the host x86 runner)";
        }
    }

    // Distinct, asymmetric, exactly-representable lanes in all three registers.
    // Asymmetry is the point: if two operands were equal, swapping their roles
    // would not change the result and an order bug would pass.
    static State seed(bool dbl) {
        State s{};
        s.mode = vitreo::oracle::kMode64;
        s.rflags = 0x202;
        s.mxcsr = 0x1F80;
        s.rip = vitreo::oracle::kCodeVa64;
        s.mem_va = vitreo::oracle::kMemVa;
        if (dbl) {
            const double v0[2] = {2.0, -3.5};      // xmm0 (dest)
            const double v1[2] = {5.0, 0.25};      // xmm1 (VEX.vvvv)
            const double v2[2] = {-7.0, 9.0};      // xmm2 (r/m)
            std::memcpy(s.xmm[0].data(), v0, 16);
            std::memcpy(s.xmm[1].data(), v1, 16);
            std::memcpy(s.xmm[2].data(), v2, 16);
        } else {
            const float v0[4] = {2.0f, -3.5f, 0.5f, 17.0f};
            const float v1[4] = {5.0f, 0.25f, -6.0f, 1.5f};
            const float v2[4] = {-7.0f, 9.0f, 2.25f, -4.0f};
            std::memcpy(s.xmm[0].data(), v0, 16);
            std::memcpy(s.xmm[1].data(), v1, 16);
            std::memcpy(s.xmm[2].data(), v2, 16);
        }
        return s;
    }

    // Counts forms the JIT actually lowered, so the sweep cannot quietly
    // degrade into an interpreter-only test.
    static int s_native;

    static void run_one(const std::string& label, bool dbl,
                        const std::vector<uint8_t>& code) {
        SCOPED_TRACE(label);
        const State in = seed(dbl);

        State oracle_out{};
        ASSERT_TRUE(vitreo::oracle::execute(in, code, &oracle_out));
        ASSERT_EQ(oracle_out.status, 0u) << "oracle snippet faulted";

        State interp_out = run_on_interpreter_64(in, code);
        ASSERT_EQ(interp_out.status, 0u);
        EXPECT_TRUE(compare(interp_out, oracle_out, CompareOpts{}).empty())
            << "interpreter <-> oracle:\n"
            << compare(interp_out, oracle_out, CompareOpts{});

        State jit_out = run_on_jit_64(in, code);
        if (jit_out.status == 7u) return;  // no native lowering yet
        ++s_native;
        ASSERT_EQ(jit_out.status, 0u);
        EXPECT_TRUE(compare(jit_out, oracle_out, CompareOpts{}).empty())
            << "JIT <-> oracle:\n"
            << compare(jit_out, oracle_out, CompareOpts{});
    }
};

int OracleFmaAll64::s_native = 0;

// VEX.128.66.0F38.Wx <op> /r, encoded as C4 E2 <W|vvvv|L|pp> <op> C2:
// dest = xmm0 (ModRM.reg), VEX.vvvv = xmm1, r/m = xmm2.
//
// Opcode byte = 0x98 + 2*variant + (scalar ? 1 : 0) + 0x10*order, where
// variant is MADD=0, MSUB=1, NMADD=2, NMSUB=3 and order is 132=0/213=1/231=2.
// W selects PD/SD over PS/SS.
TEST_F(OracleFmaAll64, EveryLoweredForm) {
    s_native = 0;
    static const char* kVariant[4] = {"VFMADD", "VFMSUB", "VFNMADD", "VFNMSUB"};
    static const char* kOrder[3] = {"132", "213", "231"};
    static const char* kType[4] = {"PS", "PD", "SS", "SD"};

    for (int variant = 0; variant < 4; ++variant) {
        for (int order = 0; order < 3; ++order) {
            for (int type = 0; type < 4; ++type) {
                const bool dbl = (type == 1 || type == 3);   // PD or SD
                const bool scalar = (type >= 2);
                const uint8_t opbyte = static_cast<uint8_t>(
                    0x98 + 2 * variant + (scalar ? 1 : 0) + 0x10 * order);
                const uint8_t byte2 = static_cast<uint8_t>(
                    (dbl ? 0x80 : 0x00) | 0x70 | 0x01);  // W | ~vvvv(xmm1) | pp=66
                run_one(std::string(kVariant[variant]) + kOrder[order] + kType[type],
                        dbl, {0xC4, 0xE2, byte2, opbyte, 0xC2});
            }
        }
    }
    // VFMADDSUB / VFMSUBADD: alternating per-lane sign, PS and PD only.
    // Opcode byte = 0x96 (ADDSUB) or 0x97 (SUBADD), + 0x10 per order.
    static const char* kAlt[2] = {"VFMADDSUB", "VFMSUBADD"};
    for (int alt = 0; alt < 2; ++alt) {
        for (int order = 0; order < 3; ++order) {
            for (int dbl = 0; dbl < 2; ++dbl) {
                const uint8_t opbyte =
                    static_cast<uint8_t>(0x96 + alt + 0x10 * order);
                const uint8_t byte2 =
                    static_cast<uint8_t>((dbl ? 0x80 : 0x00) | 0x70 | 0x01);
                run_one(std::string(kAlt[alt]) + kOrder[order] +
                            (dbl ? "PD" : "PS"),
                        dbl != 0, {0xC4, 0xE2, byte2, opbyte, 0xC2});
            }
        }
    }

    // All 48 + 12 must be natively lowered, not delegated. Without this the sweep
    // would keep passing if a lowering were removed -- the delegate produces
    // the same answers, just slowly.
    EXPECT_EQ(s_native, 60) << "some FMA forms fell back to the interpreter";
}

}  // namespace
