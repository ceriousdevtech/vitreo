// SPDX-License-Identifier: LGPL-2.1-only
// Immediate-space sweep for the AVX/AVX2 permutes and blends.
//
// VPBLENDD, VPERM2F128/I128, VPERMQ and VPERMPD all take an imm8 that selects
// lanes, and their lowerings emit a *different instruction sequence per
// immediate*. Testing one or two immediates therefore proves almost nothing --
// the same lesson the FMA sweep taught, where sampling missed an operand-order
// bug in half the forms. This walks all 256 immediates for each.
//
// Source registers hold distinct values in every lane so that any wrong lane
// selection changes the result.

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

class OracleAvxPermSweep64 : public ::testing::Test {
protected:
    void SetUp() override {
        if (!vitreo::oracle::available()) {
            GTEST_SKIP() << "oracle runner not available (requires the host x86 runner)";
        }
    }

    static State seed() {
        State s{};
        s.mode = vitreo::oracle::kMode64;
        s.rflags = 0x202;
        s.mxcsr = 0x1F80;
        s.rip = vitreo::oracle::kCodeVa64;
        s.mem_va = vitreo::oracle::kMemVa;
        // Every dword distinct across xmm0/1/2 and both 128-bit halves, so a
        // misrouted lane cannot coincidentally match.
        for (int r = 0; r < 3; ++r) {
            for (int b = 0; b < 16; ++b) {
                s.xmm[r][b] = static_cast<uint8_t>(0x10 * (r + 1) + b);
            }
        }
        return s;
    }

    static int native;

    static void run_one(const std::string& label,
                        const std::vector<uint8_t>& code) {
        SCOPED_TRACE(label);
        State in = seed();
        // Give the ymm high halves distinct contents too: the oracle seeds
        // them from xmm[8..] in its own protocol, so use registers 8-10.
        for (int r = 8; r < 11; ++r) {
            for (int b = 0; b < 16; ++b) {
                in.xmm[r][b] = static_cast<uint8_t>(0xA0 + 0x10 * (r - 8) + b);
            }
        }

        State oracle_out{};
        ASSERT_TRUE(vitreo::oracle::execute(in, code, &oracle_out));
        ASSERT_EQ(oracle_out.status, 0u) << "oracle snippet faulted";

        State interp_out = run_on_interpreter_64(in, code);
        ASSERT_EQ(interp_out.status, 0u);
        EXPECT_TRUE(compare(interp_out, oracle_out, CompareOpts{}).empty())
            << "interpreter <-> oracle:\n"
            << compare(interp_out, oracle_out, CompareOpts{});

        State jit_out = run_on_jit_64(in, code);
        if (jit_out.status == 7u) return;
        ++native;
        ASSERT_EQ(jit_out.status, 0u);
        EXPECT_TRUE(compare(jit_out, oracle_out, CompareOpts{}).empty())
            << "JIT <-> oracle:\n"
            << compare(jit_out, oracle_out, CompareOpts{});
    }
};

int OracleAvxPermSweep64::native = 0;

// VPERMQ ymm0, ymm1, imm8 — C4 E3 FD 00 C1 ib (VEX.256.66.0F3A.W1).
//
// byte2 is 0xFD, not 0xF5: these take no VEX.vvvv operand, so the field must
// be 1111b. Encoding it as 1110b (i.e. naming xmm1) is reserved -- Rosetta
// accepts it anyway, but real silicon raises #UD, which is how this was
// caught. VPERMPD is the same shape at opcode 01.
TEST_F(OracleAvxPermSweep64, VpermqAllImmediates) {
    native = 0;
    for (int imm = 0; imm < 256; ++imm) {
        run_one("VPERMQ imm=" + std::to_string(imm),
                {0xC4, 0xE3, 0xFD, 0x00, 0xC1, static_cast<uint8_t>(imm)});
    }
    EXPECT_EQ(native, 256) << "VPERMQ fell back to the interpreter";
}

TEST_F(OracleAvxPermSweep64, VpermpdAllImmediates) {
    native = 0;
    for (int imm = 0; imm < 256; ++imm) {
        run_one("VPERMPD imm=" + std::to_string(imm),
                {0xC4, 0xE3, 0xFD, 0x01, 0xC1, static_cast<uint8_t>(imm)});
    }
    EXPECT_EQ(native, 256) << "VPERMPD fell back to the interpreter";
}

// VPERM2I128 ymm0, ymm1, ymm2, imm8 — C4 E3 75 46 C2 ib. Bits 3 and 7 zero
// the corresponding output lane, which is the case a naive lowering forgets.
TEST_F(OracleAvxPermSweep64, Vperm2i128AllImmediates) {
    native = 0;
    for (int imm = 0; imm < 256; ++imm) {
        run_one("VPERM2I128 imm=" + std::to_string(imm),
                {0xC4, 0xE3, 0x75, 0x46, 0xC2, static_cast<uint8_t>(imm)});
    }
    EXPECT_EQ(native, 256) << "VPERM2I128 fell back to the interpreter";
}

TEST_F(OracleAvxPermSweep64, Vperm2f128AllImmediates) {
    native = 0;
    for (int imm = 0; imm < 256; ++imm) {
        run_one("VPERM2F128 imm=" + std::to_string(imm),
                {0xC4, 0xE3, 0x75, 0x06, 0xC2, static_cast<uint8_t>(imm)});
    }
    EXPECT_EQ(native, 256) << "VPERM2F128 fell back to the interpreter";
}

// VPBLENDD xmm0, xmm1, xmm2, imm8 — C4 E3 71 02 C2 ib (only imm[3:0] used).
TEST_F(OracleAvxPermSweep64, VpblenddXmmAllImmediates) {
    native = 0;
    for (int imm = 0; imm < 256; ++imm) {
        run_one("VPBLENDD xmm imm=" + std::to_string(imm),
                {0xC4, 0xE3, 0x71, 0x02, 0xC2, static_cast<uint8_t>(imm)});
    }
    EXPECT_EQ(native, 256) << "VPBLENDD xmm fell back to the interpreter";
}

// VPBLENDD ymm0, ymm1, ymm2, imm8 — C4 E3 75 02 C2 ib (all 8 bits used).
TEST_F(OracleAvxPermSweep64, VpblenddYmmAllImmediates) {
    native = 0;
    for (int imm = 0; imm < 256; ++imm) {
        run_one("VPBLENDD ymm imm=" + std::to_string(imm),
                {0xC4, 0xE3, 0x75, 0x02, 0xC2, static_cast<uint8_t>(imm)});
    }
    EXPECT_EQ(native, 256) << "VPBLENDD ymm fell back to the interpreter";
}

// ---------------------------------------------------------------------------
// AVX2 variable shifts. The interesting inputs are the counts, not the data:
// x86 counts are full-width unsigned and anything >= the element width gives 0
// (logical) or all sign bits (arithmetic), while ARM64's USHL/SSHL read a
// SIGNED byte per element. A count of 0x100 therefore means "shift by 256" on
// x86 and "shift by 0" on ARM if it is not clamped first.
// ---------------------------------------------------------------------------

// VPERMILPS/PD immediate: within-lane permute, one INS per output element,
// so again a different sequence per immediate. VEX.66.0F3A.W0 04 (PS) / 05
// (PD); byte2 has vvvv = 1111 because these take no vvvv operand.
TEST_F(OracleAvxPermSweep64, VpermilpsImmAllImmediates) {
    native = 0;
    for (int imm = 0; imm < 256; ++imm) {
        run_one("VPERMILPS xmm imm=" + std::to_string(imm),
                {0xC4, 0xE3, 0x79, 0x04, 0xC1, static_cast<uint8_t>(imm)});
        run_one("VPERMILPS ymm imm=" + std::to_string(imm),
                {0xC4, 0xE3, 0x7D, 0x04, 0xC1, static_cast<uint8_t>(imm)});
    }
    EXPECT_EQ(native, 512) << "VPERMILPS imm fell back";
}

TEST_F(OracleAvxPermSweep64, VpermilpdImmAllImmediates) {
    native = 0;
    for (int imm = 0; imm < 256; ++imm) {
        run_one("VPERMILPD xmm imm=" + std::to_string(imm),
                {0xC4, 0xE3, 0x79, 0x05, 0xC1, static_cast<uint8_t>(imm)});
        run_one("VPERMILPD ymm imm=" + std::to_string(imm),
                {0xC4, 0xE3, 0x7D, 0x05, 0xC1, static_cast<uint8_t>(imm)});
    }
    EXPECT_EQ(native, 512) << "VPERMILPD imm fell back";
}

class OracleAvxVarShift64 : public OracleAvxPermSweep64 {
protected:
    // Per-lane counts placed in xmm2, source data in xmm1.
    static State shift_seed(const uint64_t counts[4], bool qword) {
        State s{};
        s.mode = vitreo::oracle::kMode64;
        s.rflags = 0x202;
        s.mxcsr = 0x1F80;
        s.rip = vitreo::oracle::kCodeVa64;
        s.mem_va = vitreo::oracle::kMemVa;
        const uint64_t data[2] = {0x8000'0001'FFFF'0003ull, 0x1234'5678'9ABC'DEF0ull};
        std::memcpy(s.xmm[1].data(), data, 16);
        if (qword) {
            std::memcpy(s.xmm[2].data(), &counts[0], 8);
            std::memcpy(s.xmm[2].data() + 8, &counts[1], 8);
        } else {
            for (int i = 0; i < 4; ++i) {
                const uint32_t c = static_cast<uint32_t>(counts[i]);
                std::memcpy(s.xmm[2].data() + i * 4, &c, 4);
            }
        }
        return s;
    }

    static void run_shift(const std::string& label, bool qword,
                          const uint64_t counts[4],
                          const std::vector<uint8_t>& code) {
        SCOPED_TRACE(label);
        const State in = shift_seed(counts, qword);
        State oracle_out{};
        ASSERT_TRUE(vitreo::oracle::execute(in, code, &oracle_out));
        ASSERT_EQ(oracle_out.status, 0u) << "oracle snippet faulted";

        State interp_out = run_on_interpreter_64(in, code);
        ASSERT_EQ(interp_out.status, 0u);
        EXPECT_TRUE(compare(interp_out, oracle_out, CompareOpts{}).empty())
            << "interpreter <-> oracle:\n"
            << compare(interp_out, oracle_out, CompareOpts{});

        State jit_out = run_on_jit_64(in, code);
        if (jit_out.status == 7u) return;
        ++native;
        ASSERT_EQ(jit_out.status, 0u);
        EXPECT_TRUE(compare(jit_out, oracle_out, CompareOpts{}).empty())
            << "JIT <-> oracle:\n"
            << compare(jit_out, oracle_out, CompareOpts{});
    }
};

TEST_F(OracleAvxVarShift64, CountEdgesAcrossAllForms) {
    // 0, 1, in-range, exactly the width, just past it, and values that are
    // benign in ARM's signed-byte reading but must saturate on x86.
    static const uint64_t kCounts[][4] = {
        {0, 1, 2, 3},
        {31, 32, 33, 64},
        {63, 64, 65, 0xFF},
        {0x100, 0x101, 0x8000'0000ull, 0xFFFF'FFFFull},
        {0x80, 0x7F, 0, 255},
        {0xFFFF'FFFF'FFFF'FFFFull, 0, 1, 31},
    };
    // VEX.66.0F38.W0/W1 <op> /r: 47 = VPSLLV, 45 = VPSRLV, 46 = VPSRAVD.
    // byte2: W | ~vvvv(xmm1)=1110 | L | pp=01.
    struct Form { const char* name; uint8_t op; bool w1; bool ymm; bool qword; };
    static const Form kForms[] = {
        {"VPSLLVD xmm", 0x47, false, false, false},
        {"VPSLLVD ymm", 0x47, false, true,  false},
        {"VPSLLVQ xmm", 0x47, true,  false, true},
        {"VPSLLVQ ymm", 0x47, true,  true,  true},
        {"VPSRLVD xmm", 0x45, false, false, false},
        {"VPSRLVD ymm", 0x45, false, true,  false},
        {"VPSRLVQ xmm", 0x45, true,  false, true},
        {"VPSRLVQ ymm", 0x45, true,  true,  true},
        {"VPSRAVD xmm", 0x46, false, false, false},
        {"VPSRAVD ymm", 0x46, false, true,  false},
    };
    native = 0;
    int expected = 0;
    for (const Form& f : kForms) {
        const uint8_t byte2 = static_cast<uint8_t>(
            (f.w1 ? 0x80 : 0x00) | 0x70 | (f.ymm ? 0x04 : 0x00) | 0x01);
        for (const auto& c : kCounts) {
            run_shift(std::string(f.name) + " counts", f.qword, c,
                      {0xC4, 0xE2, byte2, f.op, 0xC2});
            ++expected;
        }
    }
    EXPECT_EQ(native, expected) << "a variable-shift form fell back";
}

// ---------------------------------------------------------------------------
// Variable-control permutes: VPERMD / VPERMPS (cross-lane) and VPERMILPS/PD
// (in-lane). The control is a runtime vector, not an immediate, so the space
// to cover is control *values* -- especially indices with bits above the mask
// width set, which x86 masks (&7 for VPERMD, &3 for VPERMILPS, bit 1 only for
// VPERMILPD) and a lowering can easily forget.
// ---------------------------------------------------------------------------

class OracleAvxVarPerm64 : public OracleAvxPermSweep64 {
protected:
    // xmm1 = control, xmm2 = source. Distinct dwords everywhere so any
    // misrouted element is visible.
    static State perm_seed(const uint32_t ctrl[8]) {
        State s{};
        s.mode = vitreo::oracle::kMode64;
        s.rflags = 0x202;
        s.mxcsr = 0x1F80;
        s.rip = vitreo::oracle::kCodeVa64;
        s.mem_va = vitreo::oracle::kMemVa;
        for (int i = 0; i < 4; ++i) {
            std::memcpy(s.xmm[1].data() + i * 4, &ctrl[i], 4);
            std::memcpy(s.xmm[9].data() + i * 4, &ctrl[i + 4], 4);
        }
        for (int i = 0; i < 4; ++i) {
            const uint32_t lo = 0xAA00'0000u + static_cast<uint32_t>(i);
            const uint32_t hi = 0xBB00'0000u + static_cast<uint32_t>(i);
            std::memcpy(s.xmm[2].data() + i * 4, &lo, 4);
            std::memcpy(s.xmm[10].data() + i * 4, &hi, 4);
        }
        return s;
    }

    static void run_perm(const std::string& label, const uint32_t ctrl[8],
                         const std::vector<uint8_t>& code) {
        SCOPED_TRACE(label);
        const State in = perm_seed(ctrl);
        State oracle_out{};
        ASSERT_TRUE(vitreo::oracle::execute(in, code, &oracle_out));
        ASSERT_EQ(oracle_out.status, 0u) << "oracle snippet faulted";

        State interp_out = run_on_interpreter_64(in, code);
        ASSERT_EQ(interp_out.status, 0u);
        EXPECT_TRUE(compare(interp_out, oracle_out, CompareOpts{}).empty())
            << "interpreter <-> oracle:\n"
            << compare(interp_out, oracle_out, CompareOpts{});

        State jit_out = run_on_jit_64(in, code);
        if (jit_out.status == 7u) return;
        ++native;
        ASSERT_EQ(jit_out.status, 0u);
        EXPECT_TRUE(compare(jit_out, oracle_out, CompareOpts{}).empty())
            << "JIT <-> oracle:\n"
            << compare(jit_out, oracle_out, CompareOpts{});
    }

    static constexpr size_t kCtrlCount = 8;
    static const uint32_t (*controls())[8] { return kCtrl; }
    static size_t control_count() { return kCtrlCount; }

private:
    static const uint32_t kCtrl[kCtrlCount][8];
};

const uint32_t OracleAvxVarPerm64::kCtrl[OracleAvxVarPerm64::kCtrlCount][8] = {
    {0, 1, 2, 3, 4, 5, 6, 7},                 // identity
    {7, 6, 5, 4, 3, 2, 1, 0},                 // reverse
    {0, 0, 0, 0, 0, 0, 0, 0},                 // broadcast lane 0
    {3, 3, 3, 3, 3, 3, 3, 3},
    {5, 2, 7, 0, 1, 6, 3, 4},                 // scattered, crosses lanes
    // High bits set: x86 masks these, a lowering that does not will index off
    // the end of the table.
    {8, 9, 10, 11, 12, 13, 14, 15},
    {0xFFFF'FFFFu, 0x8000'0000u, 0x0000'00F8u, 3, 0x7FFF'FFFFu, 2, 0x1234'5678u, 1},
    {0x100, 0x101, 0x102, 0x103, 0x104, 0x105, 0x106, 0x107},
};

TEST_F(OracleAvxVarPerm64, VpermdAndVpermps) {
    native = 0;
    int expected = 0;
    for (size_t i = 0; i < control_count(); ++i) {
        // VPERMD ymm0, ymm1, ymm2 — C4 E2 75 36 C2 (indices in VEX.vvvv)
        run_perm("VPERMD", controls()[i], {0xC4, 0xE2, 0x75, 0x36, 0xC2});
        // VPERMPS ymm0, ymm1, ymm2 — C4 E2 75 16 C2
        run_perm("VPERMPS", controls()[i], {0xC4, 0xE2, 0x75, 0x16, 0xC2});
        expected += 2;
    }
    EXPECT_EQ(native, expected) << "VPERMD/VPERMPS fell back";
}

TEST_F(OracleAvxVarPerm64, VpermilVariable) {
    native = 0;
    int expected = 0;
    for (size_t i = 0; i < control_count(); ++i) {
        // VPERMILPS xmm0, xmm1, xmm2 — C4 E2 71 0C C2 (control is r/m)
        run_perm("VPERMILPS xmm", controls()[i], {0xC4, 0xE2, 0x71, 0x0C, 0xC2});
        run_perm("VPERMILPS ymm", controls()[i], {0xC4, 0xE2, 0x75, 0x0C, 0xC2});
        run_perm("VPERMILPD xmm", controls()[i], {0xC4, 0xE2, 0x71, 0x0D, 0xC2});
        run_perm("VPERMILPD ymm", controls()[i], {0xC4, 0xE2, 0x75, 0x0D, 0xC2});
        expected += 4;
    }
    EXPECT_EQ(native, expected) << "VPERMIL variable fell back";
}

// ---------------------------------------------------------------------------
// AVX2 gathers: all eight index/element shapes, across mask patterns, scales,
// and index values including NEGATIVE ones (indices are sign-extended before
// scaling, so a plain zero-extending load turns -1 into a 4-billion-byte
// offset).
// ---------------------------------------------------------------------------

class OracleGather64 : public OracleAvxPermSweep64 {
protected:
    // dest=xmm0, index=xmm1, mask=xmm2 (VEX.vvvv), base=RDI. The base is
    // placed mid-page so negative indices stay in bounds.
    static State gather_seed(const int64_t idx[4], const uint64_t mask[4],
                             bool idx_q, bool elem_q) {
        State s{};
        s.mode = vitreo::oracle::kMode64;
        s.rflags = 0x202;
        s.mxcsr = 0x1F80;
        s.rip = vitreo::oracle::kCodeVa64;
        s.mem_va = vitreo::oracle::kMemVa;
        s.gpr[7] = vitreo::oracle::kMemVa + 128;   // RDI
        for (int i = 0; i < 4; ++i) {
            if (idx_q) {
                std::memcpy(s.xmm[1].data() + i * 8, &idx[i], 8);
            } else {
                const int32_t v = static_cast<int32_t>(idx[i]);
                std::memcpy(s.xmm[1].data() + i * 4, &v, 4);
            }
            if (elem_q) {
                std::memcpy(s.xmm[2].data() + i * 8, &mask[i], 8);
            } else {
                const uint32_t m = static_cast<uint32_t>(mask[i] >> 32);
                std::memcpy(s.xmm[2].data() + i * 4, &m, 4);
            }
        }
        // Distinct payload across the scratch page.
        for (size_t b = 0; b < s.mem.size(); ++b) {
            s.mem[b] = static_cast<uint8_t>(0x40 + (b & 0x3F));
        }
        return s;
    }

    static void run_gather(const std::string& label, const int64_t idx[4],
                           const uint64_t mask[4], bool idx_q, bool elem_q,
                           const std::vector<uint8_t>& code) {
        SCOPED_TRACE(label);
        const State in = gather_seed(idx, mask, idx_q, elem_q);
        State oracle_out{};
        ASSERT_TRUE(vitreo::oracle::execute(in, code, &oracle_out));
        ASSERT_EQ(oracle_out.status, 0u) << "oracle snippet faulted";

        State interp_out = run_on_interpreter_64(in, code);
        ASSERT_EQ(interp_out.status, 0u);
        EXPECT_TRUE(compare(interp_out, oracle_out, CompareOpts{}).empty())
            << "interpreter <-> oracle:\n"
            << compare(interp_out, oracle_out, CompareOpts{});

        State jit_out = run_on_jit_64(in, code);
        if (jit_out.status == 7u) return;
        ++native;
        ASSERT_EQ(jit_out.status, 0u);
        EXPECT_TRUE(compare(jit_out, oracle_out, CompareOpts{}).empty())
            << "JIT <-> oracle:\n"
            << compare(jit_out, oracle_out, CompareOpts{});
    }
};

TEST_F(OracleGather64, AllShapesMasksAndScales) {
    constexpr uint64_t kOn = 0x8000'0000'0000'0000ull;
    static const uint64_t kMasks[][4] = {
        {kOn, kOn, kOn, kOn},     // all enabled
        {0, 0, 0, 0},             // none enabled: dest must be untouched
        {kOn, 0, kOn, 0},         // alternating
        {0, kOn, 0, kOn},
        {kOn, 0, 0, 0},
    };
    static const int64_t kIdx[][4] = {
        {0, 1, 2, 3},
        {3, 2, 1, 0},
        {-1, -2, 0, 1},           // negative: must sign-extend
        {-8, 7, -4, 2},
        {0, 0, 0, 0},
    };
    // opcode, W1?, name
    struct Form { uint8_t op; bool w1; bool idx_q; bool elem_q; const char* name; };
    static const Form kForms[] = {
        {0x92, false, false, false, "VGATHERDPS"},
        {0x90, false, false, false, "VPGATHERDD"},
        {0x93, false, true,  false, "VGATHERQPS"},
        {0x91, false, true,  false, "VPGATHERQD"},
        {0x92, true,  false, true,  "VGATHERDPD"},
        {0x90, true,  false, true,  "VPGATHERDQ"},
        {0x93, true,  true,  true,  "VGATHERQPD"},
        {0x91, true,  true,  true,  "VPGATHERQQ"},
    };
    static const uint8_t kSib[] = {0x0F, 0x4F, 0x8F, 0xCF};  // scale 1,2,4,8

    native = 0;
    int expected = 0;
    for (const Form& f : kForms) {
        for (int l = 0; l < 2; ++l) {               // VEX.128 and VEX.256
            const uint8_t byte2 = static_cast<uint8_t>(
                (f.w1 ? 0x80 : 0x00) | 0x68 | (l ? 0x04 : 0x00) | 0x01);
            for (const auto& m : kMasks) {
                for (const auto& ix : kIdx) {
                    for (uint8_t sib : kSib) {
                        run_gather(f.name, ix, m, f.idx_q, f.elem_q,
                                   {0xC4, 0xE2, byte2, f.op, 0x04, sib});
                        ++expected;
                    }
                }
            }
        }
    }
    EXPECT_EQ(native, expected) << "a gather form fell back to the interpreter";
}

}  // namespace
