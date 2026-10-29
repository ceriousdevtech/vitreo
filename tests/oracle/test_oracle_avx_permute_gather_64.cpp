// SPDX-License-Identifier: LGPL-2.1-only
// x86-64 interpreter/JIT <-> Oracle tests for AVX/AVX2:
// VPERMILPS/PD, VTESTPS/PD,
// VMASKMOVPS/PD, and the gather forms with qword indices and/or qword
// elements.
//
// These landed with hand-written unit tests asserting a reading of the Intel
// SDM. That is exactly the validation this oracle exists to replace: in this
// same work a zero-count rotate was "obviously" a no-op by the SDM's wording,
// and real silicon disagreed. Every instruction here is now checked against
// the real thing.
//
// Same harness and fixture conventions as test_oracle_avx2_64.cpp. The fixture
// name is unique so this TU does not ODR-collide with the others.

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

class OracleAvxPermGather64 : public ::testing::Test {
protected:
    void SetUp() override {
        if (!vitreo::oracle::available()) {
            GTEST_SKIP() << "oracle runner not available (requires the host x86 runner)";
        }
    }

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
            0x0000'DEAD'BEEF'0BADull, vitreo::oracle::kMemVa,  // RDI = scratch
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

    // Set XMM[reg] to an explicit little-endian qword pair.
    static void set_xmm_q(State& s, int reg, uint64_t lo, uint64_t hi) {
        std::memcpy(s.xmm[reg].data(), &lo, 8);
        std::memcpy(s.xmm[reg].data() + 8, &hi, 8);
    }

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
            << "interpreter <-> oracle divergence:\n" << interp_diff;

        State jit_out = run_on_jit_64(in, code);
        if (jit_out.status == 7u) {
            GTEST_SKIP() << "JIT has no native lowering for this snippet";
        }
        ASSERT_EQ(jit_out.status, 0u) << "JIT snippet faulted";
        std::string jit_diff = compare(jit_out, oracle_out, opts);
        EXPECT_TRUE(jit_diff.empty())
            << "JIT <-> oracle divergence:\n" << jit_diff;
    }
};

// ---------------------------------------------------------------------------
// VPERMILPS / VPERMILPD
// ---------------------------------------------------------------------------

// VPERMILPS xmm0, xmm1, 0x1B — C4 E3 79 04 C1 1B (full reverse within a lane).
TEST_F(OracleAvxPermGather64, VpermilpsImmXmm) {
    run_triplet(seed_state(), {0xC4, 0xE3, 0x79, 0x04, 0xC1, 0x1B});
}

// VPERMILPS ymm0, ymm1, 0x00 — C4 E3 7D 04 C1 00. The selector is applied to
// each 128-bit lane independently; this is the case that catches a lowering
// that treats the control as crossing lanes.
TEST_F(OracleAvxPermGather64, VpermilpsImmYmmIsPerLane) {
    run_triplet(seed_state(), {0xC4, 0xE3, 0x7D, 0x04, 0xC1, 0x00});
}

// VPERMILPS xmm0, xmm1, xmm2 — C4 E2 71 0C C2. Variable control in xmm2; only
// the low 2 bits of each control dword participate.
TEST_F(OracleAvxPermGather64, VpermilpsVarXmm) {
    run_triplet(seed_state(), {0xC4, 0xE2, 0x71, 0x0C, 0xC2});
}

TEST_F(OracleAvxPermGather64, VpermilpsVarYmm) {
    run_triplet(seed_state(), {0xC4, 0xE2, 0x75, 0x0C, 0xC2});
}

// VPERMILPD xmm0, xmm1, 0x01 — C4 E3 79 05 C1 01 (swap the two qwords).
TEST_F(OracleAvxPermGather64, VpermilpdImmXmm) {
    run_triplet(seed_state(), {0xC4, 0xE3, 0x79, 0x05, 0xC1, 0x01});
}

TEST_F(OracleAvxPermGather64, VpermilpdImmYmm) {
    run_triplet(seed_state(), {0xC4, 0xE3, 0x7D, 0x05, 0xC1, 0x06});
}

// VPERMILPD xmm0, xmm1, xmm2 — C4 E2 71 0D C2. The selector is bit *1* of each
// control qword, not bit 0; the seed leaves both patterns present.
TEST_F(OracleAvxPermGather64, VpermilpdVarXmm) {
    State s = seed_state();
    set_xmm_q(s, 2, 2, 1);  // bit1 set -> elem 1; bit1 clear -> elem 0
    run_triplet(s, {0xC4, 0xE2, 0x71, 0x0D, 0xC2});
}

TEST_F(OracleAvxPermGather64, VpermilpdVarYmm) {
    run_triplet(seed_state(), {0xC4, 0xE2, 0x75, 0x0D, 0xC2});
}

// ---------------------------------------------------------------------------
// VTESTPS / VTESTPD — sign bits only; ZF and CF written, other flags cleared.
//
// These compare ZF and CF only. Rosetta does not clear OF/AF/PF/SF for the
// TEST family (see kTestFlagsRosettaMask), so comparing those four here would
// assert a Rosetta defect rather than the Intel contract. Confirmed on real
// silicon on Windows: hardware clears them, and so does
// Vitreo. The clearing behavior is pinned by
// tests/cpu/test_avx_permute_gather.cpp instead.
// ---------------------------------------------------------------------------

// ZF|CF only — the subset the Rosetta oracle answers correctly here.
static vitreo::oracle::contract::CompareOpts test_flag_opts() {
    vitreo::oracle::contract::CompareOpts o{};
    o.flags_mask = vitreo::oracle::contract::kTestFlagsRosettaMask;
    return o;
}

// VTESTPS xmm0, xmm1 — C4 E2 79 0E C1. Seed values have clear sign bits, so
// this pins the all-clear case (ZF=1, CF=1).
TEST_F(OracleAvxPermGather64, VtestpsAllSignBitsClear) {
    State s = seed_state();
    s.rflags = 0x202 | vitreo::oracle::contract::kArithFlagsMask;
    run_triplet(s, {0xC4, 0xE2, 0x79, 0x0E, 0xC1}, test_flag_opts());
}

// Overlapping sign bits: ZF must clear while CF stays set.
TEST_F(OracleAvxPermGather64, VtestpsOverlappingSignBits) {
    State s = seed_state();
    s.rflags = 0x202 | vitreo::oracle::contract::kArithFlagsMask;
    set_xmm_q(s, 0, 0x8000'0000'0000'0000ull, 0);
    set_xmm_q(s, 1, 0x8000'0000'0000'0000ull, 0);
    run_triplet(s, {0xC4, 0xE2, 0x79, 0x0E, 0xC1}, test_flag_opts());
}

// Disjoint sign bits: ZF set, CF clear. Also the case that catches an
// implementation testing whole elements instead of just the sign bit.
TEST_F(OracleAvxPermGather64, VtestpsDisjointSignBits) {
    State s = seed_state();
    s.rflags = 0x202 | vitreo::oracle::contract::kArithFlagsMask;
    set_xmm_q(s, 0, 0x8000'0000'7FFF'FFFFull, 0x7FFF'FFFF'7FFF'FFFFull);
    set_xmm_q(s, 1, 0x7FFF'FFFF'8000'0000ull, 0x7FFF'FFFF'7FFF'FFFFull);
    run_triplet(s, {0xC4, 0xE2, 0x79, 0x0E, 0xC1}, test_flag_opts());
}

TEST_F(OracleAvxPermGather64, VtestpsYmm) {
    State s = seed_state();
    s.rflags = 0x202 | vitreo::oracle::contract::kArithFlagsMask;
    set_xmm_q(s, 0, 0x8000'0000'0000'0000ull, 0);
    set_xmm_q(s, 1, 0x8000'0000'0000'0000ull, 0);
    run_triplet(s, {0xC4, 0xE2, 0x7D, 0x0E, 0xC1}, test_flag_opts());
}

// VTESTPD xmm0, xmm1 — C4 E2 79 0F C1. Qword sign bits.
TEST_F(OracleAvxPermGather64, VtestpdDisjointSignBits) {
    State s = seed_state();
    s.rflags = 0x202 | vitreo::oracle::contract::kArithFlagsMask;
    set_xmm_q(s, 0, 0x8000'0000'0000'0000ull, 0);
    set_xmm_q(s, 1, 0, 0x8000'0000'0000'0000ull);
    run_triplet(s, {0xC4, 0xE2, 0x79, 0x0F, 0xC1}, test_flag_opts());
}

TEST_F(OracleAvxPermGather64, VtestpdYmm) {
    State s = seed_state();
    s.rflags = 0x202 | vitreo::oracle::contract::kArithFlagsMask;
    set_xmm_q(s, 0, 0x8000'0000'0000'0000ull, 0x8000'0000'0000'0000ull);
    set_xmm_q(s, 1, 0x8000'0000'0000'0000ull, 0);
    run_triplet(s, {0xC4, 0xE2, 0x7D, 0x0F, 0xC1}, test_flag_opts());
}

// ---------------------------------------------------------------------------
// VMASKMOVPS / VMASKMOVPD — mask in VEX.vvvv, memory operand at [RDI]
// ---------------------------------------------------------------------------

// VMASKMOVPS xmm0, xmm1, [rdi] — C4 E2 71 2C 07. Mixed mask: masked-out lanes
// must be zeroed in the destination, not left alone.
TEST_F(OracleAvxPermGather64, VmaskmovpsLoadXmm) {
    State s = seed_state();
    set_xmm_q(s, 1, 0x0000'0000'8000'0000ull, 0x8000'0000'0000'0000ull);
    run_triplet(s, {0xC4, 0xE2, 0x71, 0x2C, 0x07});
}

TEST_F(OracleAvxPermGather64, VmaskmovpsLoadYmm) {
    State s = seed_state();
    set_xmm_q(s, 1, 0x0000'0000'8000'0000ull, 0x8000'0000'0000'0000ull);
    run_triplet(s, {0xC4, 0xE2, 0x75, 0x2C, 0x07});
}

// VMASKMOVPS [rdi], xmm1, xmm0 — C4 E2 71 2E 07. Masked-out lanes must leave
// memory untouched.
TEST_F(OracleAvxPermGather64, VmaskmovpsStoreXmm) {
    State s = seed_state();
    set_xmm_q(s, 1, 0x0000'0000'8000'0000ull, 0x8000'0000'0000'0000ull);
    run_triplet(s, {0xC4, 0xE2, 0x71, 0x2E, 0x07});
}

// VMASKMOVPD xmm0, xmm1, [rdi] — C4 E2 71 2D 07 (qword lanes).
TEST_F(OracleAvxPermGather64, VmaskmovpdLoadXmm) {
    State s = seed_state();
    set_xmm_q(s, 1, 0x8000'0000'0000'0000ull, 0);
    run_triplet(s, {0xC4, 0xE2, 0x71, 0x2D, 0x07});
}

// VMASKMOVPD [rdi], xmm1, xmm0 — C4 E2 71 2F 07.
TEST_F(OracleAvxPermGather64, VmaskmovpdStoreXmm) {
    State s = seed_state();
    set_xmm_q(s, 1, 0x8000'0000'0000'0000ull, 0);
    run_triplet(s, {0xC4, 0xE2, 0x71, 0x2F, 0x07});
}

// An all-zero mask must move nothing at all — for a store that means memory is
// completely unchanged, and it is the case where a faulting-address bug would
// otherwise be masked by the data comparison passing.
TEST_F(OracleAvxPermGather64, VmaskmovpsStoreZeroMaskWritesNothing) {
    State s = seed_state();
    set_xmm_q(s, 1, 0, 0);
    run_triplet(s, {0xC4, 0xE2, 0x71, 0x2E, 0x07});
}

// ---------------------------------------------------------------------------
// Gathers. Destination xmm0, index xmm1, mask xmm2 — all distinct, as the SDM
// requires. Base is RDI = kMemVa. Indices are kept small so every computed
// address stays inside the scratch page.
// ---------------------------------------------------------------------------

// VGATHERQPS xmm0, [rdi + xmm1*4], xmm2 — C4 E2 69 93 04 8F
// SIB 0x8F: scale field 10b == x4, index xmm1, base RDI.
TEST_F(OracleAvxPermGather64, VgatherqpsQwordIndexDwordElement) {
    State s = seed_state();
    set_xmm_q(s, 1, 3, 1);  // qword indices
    set_xmm_q(s, 2, 0x8000'0000'8000'0000ull, 0);  // both dword lanes enabled
    run_triplet(s, {0xC4, 0xE2, 0x69, 0x93, 0x04, 0x8F});
}

// The same shape with one mask bit clear: that destination element must keep
// its previous contents while the mask register is still fully zeroed.
TEST_F(OracleAvxPermGather64, VgatherqpsPartialMask) {
    State s = seed_state();
    set_xmm_q(s, 1, 3, 1);
    set_xmm_q(s, 2, 0x0000'0000'8000'0000ull, 0);
    run_triplet(s, {0xC4, 0xE2, 0x69, 0x93, 0x04, 0x8F});
}

// VPGATHERQD xmm0, [rdi + xmm1*4], xmm2 — C4 E2 69 91 04 8F
TEST_F(OracleAvxPermGather64, VpgatherqdQwordIndexDwordElement) {
    State s = seed_state();
    set_xmm_q(s, 1, 2, 0);
    set_xmm_q(s, 2, 0x8000'0000'8000'0000ull, 0);
    run_triplet(s, {0xC4, 0xE2, 0x69, 0x91, 0x04, 0x8F});
}

// VGATHERDPD xmm0, [rdi + xmm1*8], xmm2 — C4 E2 E9 92 04 CF
// VEX.W1 selects qword elements; SIB 0xCF is scale x8. Only the low two index
// dwords are consumed.
TEST_F(OracleAvxPermGather64, VgatherdpdDwordIndexQwordElement) {
    State s = seed_state();
    set_xmm_q(s, 1, 0x0000'0003'0000'0001ull, 0);  // dword indices 1 and 3
    set_xmm_q(s, 2, 0x8000'0000'0000'0000ull, 0x8000'0000'0000'0000ull);
    run_triplet(s, {0xC4, 0xE2, 0xE9, 0x92, 0x04, 0xCF});
}

// VPGATHERDQ xmm0, [rdi + xmm1*8], xmm2 — C4 E2 E9 90 04 CF
TEST_F(OracleAvxPermGather64, VpgatherdqDwordIndexQwordElement) {
    State s = seed_state();
    set_xmm_q(s, 1, 0x0000'0002'0000'0000ull, 0);
    set_xmm_q(s, 2, 0x8000'0000'0000'0000ull, 0x8000'0000'0000'0000ull);
    run_triplet(s, {0xC4, 0xE2, 0xE9, 0x90, 0x04, 0xCF});
}

// VGATHERQPD xmm0, [rdi + xmm1*8], xmm2 — C4 E2 E9 93 04 CF
TEST_F(OracleAvxPermGather64, VgatherqpdQwordIndexQwordElement) {
    State s = seed_state();
    set_xmm_q(s, 1, 1, 3);
    set_xmm_q(s, 2, 0x8000'0000'0000'0000ull, 0x8000'0000'0000'0000ull);
    run_triplet(s, {0xC4, 0xE2, 0xE9, 0x93, 0x04, 0xCF});
}

// VPGATHERQQ xmm0, [rdi + xmm1*8], xmm2 — C4 E2 E9 91 04 CF
TEST_F(OracleAvxPermGather64, VpgatherqqQwordIndexQwordElement) {
    State s = seed_state();
    set_xmm_q(s, 1, 2, 0);
    set_xmm_q(s, 2, 0x8000'0000'0000'0000ull, 0x8000'0000'0000'0000ull);
    run_triplet(s, {0xC4, 0xE2, 0xE9, 0x91, 0x04, 0xCF});
}

// A fully-clear mask gathers nothing: the destination keeps every element and
// the mask register still ends up zeroed.
TEST_F(OracleAvxPermGather64, VpgatherqqZeroMaskGathersNothing) {
    State s = seed_state();
    set_xmm_q(s, 1, 2, 0);
    set_xmm_q(s, 2, 0, 0);
    run_triplet(s, {0xC4, 0xE2, 0xE9, 0x91, 0x04, 0xCF});
}

// Negative index: index elements are sign-extended before scaling, so a
// negative value must address *below* the base rather than wrapping to a huge
// unsigned offset. The base is advanced so the computed address stays in page.
TEST_F(OracleAvxPermGather64, VpgatherqqNegativeIndexSignExtends) {
    State s = seed_state();
    s.gpr[7] = vitreo::oracle::kMemVa + 64;  // RDI
    set_xmm_q(s, 1, static_cast<uint64_t>(-2ll), 1);
    set_xmm_q(s, 2, 0x8000'0000'0000'0000ull, 0x8000'0000'0000'0000ull);
    run_triplet(s, {0xC4, 0xE2, 0xE9, 0x91, 0x04, 0xCF});
}

}  // namespace
