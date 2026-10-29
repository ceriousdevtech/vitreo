// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// AVX/AVX2 instructions that CPUID advertises but the decoder used to reject:
// VPERMILPS/PD, VTESTPS/PD, VMASKMOVPS/PD, and the gather forms with qword
// indices and/or qword elements.
//
// Contract source: Intel SDM Vol. 2 (VPERMILPS, VPERMILPD, VTESTPS, VTESTPD,
// VMASKMOV, VGATHERQPS, VGATHERDPD, VGATHERQPD and the VPGATHER* forms).

#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include <gtest/gtest.h>
#include <memory>
#include <vector>

using namespace vitreo;
using namespace vitreo::cpu;

namespace {

constexpr u64 kCodeBase = 0x1000;
constexpr u64 kDataBase = 0x2000;

DecodedInstruction decode64(const std::vector<u8>& bytes) {
    MemoryByteReader reader(bytes.data(), bytes.size());
    DecodedInstruction inst;
    EXPECT_EQ(decode_instruction(reader, DecodeMode::Mode64, inst),
              DecodeError::Success);
    return inst;
}

class AvxPermuteGatherTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        ASSERT_TRUE(m_memory->map_region(0x1000, 0x20000,
                                         MemoryProtection::ReadWrite));
    }

    ExecResult run(const std::vector<u8>& code, CpuState& cpu) {
        EXPECT_TRUE(m_memory->write_bytes(kCodeBase, code.data(), code.size()));
        cpu.rip = kCodeBase;
        Interpreter interp(cpu, *m_memory);
        return interp.step();
    }

    static CpuState fresh() {
        CpuState cpu;
        cpu.init_long_mode();
        cpu.gpr.set64(static_cast<u8>(RegIndex::RSP), 0x10000);
        return cpu;
    }

    std::unique_ptr<GuestMemory> m_memory;
};

}  // namespace

// ===========================================================================
// VPERMILPS / VPERMILPD
// ===========================================================================

// VEX.128.66.0F3A.W0 04 /r ib — vpermilps xmm0, xmm1, imm8
TEST_F(AvxPermuteGatherTest, VpermilpsImmReversesLane) {
    auto cpu = fresh();
    for (int i = 0; i < 4; ++i) cpu.sse.xmm[1].u32_[i] = static_cast<u32>(i + 1);
    cpu.avx.ymm_high[0].u32_[0] = 0xDEADBEEFu;

    // imm8 = 0x1B selects 3,2,1,0 — the full reverse.
    ASSERT_EQ(run({0xC4, 0xE3, 0x79, 0x04, 0xC1, 0x1B}, cpu),
              ExecResult::Continue);

    EXPECT_EQ(cpu.sse.xmm[0].u32_[0], 4u);
    EXPECT_EQ(cpu.sse.xmm[0].u32_[1], 3u);
    EXPECT_EQ(cpu.sse.xmm[0].u32_[2], 2u);
    EXPECT_EQ(cpu.sse.xmm[0].u32_[3], 1u);
    // A 128-bit VEX form zeroes the upper half of the destination.
    EXPECT_EQ(cpu.avx.ymm_high[0].u32_[0], 0u);
}

// VEX.128.66.0F38.W0 0C /r — vpermilps xmm0, xmm1, xmm2 (control in xmm2)
TEST_F(AvxPermuteGatherTest, VpermilpsVarUsesControlVector) {
    auto cpu = fresh();
    cpu.sse.xmm[1].u32_[0] = 10; cpu.sse.xmm[1].u32_[1] = 20;
    cpu.sse.xmm[1].u32_[2] = 30; cpu.sse.xmm[1].u32_[3] = 40;
    // Only the low 2 bits of each control dword are used.
    cpu.sse.xmm[2].u32_[0] = 3; cpu.sse.xmm[2].u32_[1] = 0x7FFFFFFFu;
    cpu.sse.xmm[2].u32_[2] = 0; cpu.sse.xmm[2].u32_[3] = 1;

    ASSERT_EQ(run({0xC4, 0xE2, 0x71, 0x0C, 0xC2}, cpu), ExecResult::Continue);

    EXPECT_EQ(cpu.sse.xmm[0].u32_[0], 40u);  // sel 3
    EXPECT_EQ(cpu.sse.xmm[0].u32_[1], 40u);  // 0x7FFFFFFF & 3 == 3
    EXPECT_EQ(cpu.sse.xmm[0].u32_[2], 10u);  // sel 0
    EXPECT_EQ(cpu.sse.xmm[0].u32_[3], 20u);  // sel 1
}

// The 256-bit form permutes each 128-bit lane independently — a selector of 0
// in the high lane picks the high lane's own element 0, never the low lane's.
TEST_F(AvxPermuteGatherTest, Vpermilps256DoesNotCrossLanes) {
    auto cpu = fresh();
    for (int i = 0; i < 4; ++i) {
        cpu.sse.xmm[1].u32_[i] = static_cast<u32>(i + 1);          // 1..4
        cpu.avx.ymm_high[1].u32_[i] = static_cast<u32>(i + 101);   // 101..104
    }
    // VEX.256: L=1 => byte2 0x79 | 0x04 = 0x7D
    ASSERT_EQ(run({0xC4, 0xE3, 0x7D, 0x04, 0xC1, 0x00}, cpu),
              ExecResult::Continue);

    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(cpu.sse.xmm[0].u32_[i], 1u);
        EXPECT_EQ(cpu.avx.ymm_high[0].u32_[i], 101u);
    }
}

// VEX.128.66.0F3A.W0 05 /r ib — vpermilpd xmm0, xmm1, imm8
TEST_F(AvxPermuteGatherTest, VpermilpdImmSwapsQwords) {
    auto cpu = fresh();
    cpu.sse.xmm[1].u64_[0] = 0x1111111111111111ull;
    cpu.sse.xmm[1].u64_[1] = 0x2222222222222222ull;

    // imm8 bit 0 selects element 0, bit 1 selects element 1: 0b01 swaps.
    ASSERT_EQ(run({0xC4, 0xE3, 0x79, 0x05, 0xC1, 0x01}, cpu),
              ExecResult::Continue);

    EXPECT_EQ(cpu.sse.xmm[0].u64_[0], 0x2222222222222222ull);
    EXPECT_EQ(cpu.sse.xmm[0].u64_[1], 0x1111111111111111ull);
}

// VEX.128.66.0F38.W0 0D /r — control is bit 1 of each qword, not bit 0.
TEST_F(AvxPermuteGatherTest, VpermilpdVarUsesBitOneOfControl) {
    auto cpu = fresh();
    cpu.sse.xmm[1].u64_[0] = 0xAAAAull;
    cpu.sse.xmm[1].u64_[1] = 0xBBBBull;
    cpu.sse.xmm[2].u64_[0] = 2;  // bit 1 set -> element 1
    cpu.sse.xmm[2].u64_[1] = 1;  // bit 1 clear -> element 0 (bit 0 ignored)

    ASSERT_EQ(run({0xC4, 0xE2, 0x71, 0x0D, 0xC2}, cpu), ExecResult::Continue);

    EXPECT_EQ(cpu.sse.xmm[0].u64_[0], 0xBBBBull);
    EXPECT_EQ(cpu.sse.xmm[0].u64_[1], 0xAAAAull);
}

// ===========================================================================
// VTESTPS / VTESTPD — sign bits only, and a full EFLAGS write
// ===========================================================================

// VEX.128.66.0F38.W0 0E /r — vtestps xmm0, xmm1
TEST_F(AvxPermuteGatherTest, VtestpsSetsZfAndCfFromSignBits) {
    auto cpu = fresh();
    // DEST sign mask 0b0001, SRC sign mask 0b0001.
    cpu.sse.xmm[0].u32_[0] = 0x80000000u;
    cpu.sse.xmm[1].u32_[0] = 0x80000000u;

    ASSERT_EQ(run({0xC4, 0xE2, 0x79, 0x0E, 0xC1}, cpu), ExecResult::Continue);

    EXPECT_FALSE(cpu.flags.zf());  // SRC AND DEST has a sign bit set
    EXPECT_TRUE(cpu.flags.cf());   // SRC AND NOT DEST has none
    EXPECT_FALSE(cpu.flags.of());
    EXPECT_FALSE(cpu.flags.sf());
    EXPECT_FALSE(cpu.flags.af());
    EXPECT_FALSE(cpu.flags.pf());
}

// Non-sign bits must not participate: a value of 0x7FFFFFFF is "clear".
TEST_F(AvxPermuteGatherTest, VtestpsIgnoresNonSignBits) {
    auto cpu = fresh();
    for (int i = 0; i < 4; ++i) {
        cpu.sse.xmm[0].u32_[i] = 0x7FFFFFFFu;
        cpu.sse.xmm[1].u32_[i] = 0x7FFFFFFFu;
    }

    ASSERT_EQ(run({0xC4, 0xE2, 0x79, 0x0E, 0xC1}, cpu), ExecResult::Continue);

    EXPECT_TRUE(cpu.flags.zf());
    EXPECT_TRUE(cpu.flags.cf());
}

// VEX.128.66.0F38.W0 0F /r — vtestpd xmm0, xmm1
TEST_F(AvxPermuteGatherTest, VtestpdUsesQwordSignBits) {
    auto cpu = fresh();
    cpu.sse.xmm[0].u64_[0] = 0x8000000000000000ull;  // DEST mask 0b01
    cpu.sse.xmm[1].u64_[1] = 0x8000000000000000ull;  // SRC  mask 0b10

    ASSERT_EQ(run({0xC4, 0xE2, 0x79, 0x0F, 0xC1}, cpu), ExecResult::Continue);

    EXPECT_TRUE(cpu.flags.zf());   // SRC AND DEST == 0
    EXPECT_FALSE(cpu.flags.cf());  // SRC AND NOT DEST != 0
}

// ===========================================================================
// VMASKMOVPS / VMASKMOVPD
// ===========================================================================

// VEX.128.66.0F38.W0 2C /r — vmaskmovps xmm0, xmm1, [rax]
TEST_F(AvxPermuteGatherTest, VmaskmovpsLoadZeroesMaskedOutLanes) {
    auto cpu = fresh();
    cpu.gpr.set64(static_cast<u8>(RegIndex::RAX), kDataBase);
    const u32 src[4] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
    ASSERT_TRUE(m_memory->write_bytes(kDataBase,
                                      reinterpret_cast<const u8*>(src), 16));
    // Mask (VEX.vvvv = xmm1): lanes 0 and 2 enabled.
    cpu.sse.xmm[1].u32_[0] = 0x80000000u;
    cpu.sse.xmm[1].u32_[2] = 0x80000000u;
    for (int i = 0; i < 4; ++i) cpu.sse.xmm[0].u32_[i] = 0xCCCCCCCCu;

    ASSERT_EQ(run({0xC4, 0xE2, 0x71, 0x2C, 0x00}, cpu), ExecResult::Continue);

    EXPECT_EQ(cpu.sse.xmm[0].u32_[0], 0x11111111u);
    EXPECT_EQ(cpu.sse.xmm[0].u32_[1], 0u);  // masked out -> zeroed, not kept
    EXPECT_EQ(cpu.sse.xmm[0].u32_[2], 0x33333333u);
    EXPECT_EQ(cpu.sse.xmm[0].u32_[3], 0u);
}

// VEX.128.66.0F38.W0 2E /r — vmaskmovps [rax], xmm1, xmm0
TEST_F(AvxPermuteGatherTest, VmaskmovpsStoreLeavesMaskedOutMemory) {
    auto cpu = fresh();
    cpu.gpr.set64(static_cast<u8>(RegIndex::RAX), kDataBase);
    const u32 initial[4] = {0x99999999u, 0x99999999u, 0x99999999u, 0x99999999u};
    ASSERT_TRUE(m_memory->write_bytes(kDataBase,
                                      reinterpret_cast<const u8*>(initial), 16));
    cpu.sse.xmm[1].u32_[1] = 0x80000000u;  // only lane 1 enabled
    for (int i = 0; i < 4; ++i) cpu.sse.xmm[0].u32_[i] = 0x55555555u;

    ASSERT_EQ(run({0xC4, 0xE2, 0x71, 0x2E, 0x00}, cpu), ExecResult::Continue);

    u32 out[4]{};
    ASSERT_TRUE(m_memory->read_bytes(kDataBase,
                                     reinterpret_cast<u8*>(out), 16));
    EXPECT_EQ(out[0], 0x99999999u);
    EXPECT_EQ(out[1], 0x55555555u);
    EXPECT_EQ(out[2], 0x99999999u);
    EXPECT_EQ(out[3], 0x99999999u);
}

// ===========================================================================
// Gathers with qword indices and/or qword elements
// ===========================================================================

// VEX.128.66.0F38.W0 93 /r — vgatherqps xmm0, [rax + xmm1*4], xmm2
// SIB 0x88: scale field 10b == 2^2 == x4, index xmm1, base rax.
TEST_F(AvxPermuteGatherTest, VgatherqpsQwordIndexDwordElement) {
    auto cpu = fresh();
    cpu.gpr.set64(static_cast<u8>(RegIndex::RAX), kDataBase);
    const u32 table[4] = {0xAAAAAAAAu, 0xBBBBBBBBu, 0xCCCCCCCCu, 0xDDDDDDDDu};
    ASSERT_TRUE(m_memory->write_bytes(kDataBase,
                                      reinterpret_cast<const u8*>(table), 16));
    cpu.sse.xmm[1].u64_[0] = 3;  // qword indices
    cpu.sse.xmm[1].u64_[1] = 1;
    cpu.sse.xmm[2].u64_[0] = 0x8000000080000000ull;  // both dword lanes enabled
    for (int i = 0; i < 4; ++i) cpu.sse.xmm[0].u32_[i] = 0xEEEEEEEEu;
    cpu.avx.ymm_high[0].u32_[0] = 0xEEEEEEEEu;

    ASSERT_EQ(run({0xC4, 0xE2, 0x69, 0x93, 0x04, 0x88}, cpu),
              ExecResult::Continue);

    EXPECT_EQ(cpu.sse.xmm[0].u32_[0], 0xDDDDDDDDu);
    EXPECT_EQ(cpu.sse.xmm[0].u32_[1], 0xBBBBBBBBu);
    // Dword-element form with 2 elements zeroes everything above them.
    EXPECT_EQ(cpu.sse.xmm[0].u32_[2], 0u);
    EXPECT_EQ(cpu.sse.xmm[0].u32_[3], 0u);
    EXPECT_EQ(cpu.avx.ymm_high[0].u32_[0], 0u);
    // The mask register is fully zeroed on completion.
    EXPECT_EQ(cpu.sse.xmm[2].u64_[0], 0ull);
}

// A clear mask bit leaves the destination element untouched.
TEST_F(AvxPermuteGatherTest, VgatherqpsSkipsMaskedOutElement) {
    auto cpu = fresh();
    cpu.gpr.set64(static_cast<u8>(RegIndex::RAX), kDataBase);
    const u32 table[4] = {0xAAAAAAAAu, 0xBBBBBBBBu, 0xCCCCCCCCu, 0xDDDDDDDDu};
    ASSERT_TRUE(m_memory->write_bytes(kDataBase,
                                      reinterpret_cast<const u8*>(table), 16));
    cpu.sse.xmm[1].u64_[0] = 0;
    cpu.sse.xmm[1].u64_[1] = 1;
    cpu.sse.xmm[2].u32_[0] = 0x80000000u;  // lane 0 only
    cpu.sse.xmm[2].u32_[1] = 0;
    cpu.sse.xmm[0].u32_[0] = 0x12345678u;
    cpu.sse.xmm[0].u32_[1] = 0x0BADF00Du;

    ASSERT_EQ(run({0xC4, 0xE2, 0x69, 0x93, 0x04, 0x88}, cpu),
              ExecResult::Continue);

    EXPECT_EQ(cpu.sse.xmm[0].u32_[0], 0xAAAAAAAAu);
    EXPECT_EQ(cpu.sse.xmm[0].u32_[1], 0x0BADF00Du);  // preserved, not zeroed
}

// VEX.128.66.0F38.W1 91 /r — vpgatherqq xmm0, [rax + xmm1*8], xmm2
TEST_F(AvxPermuteGatherTest, VpgatherqqQwordIndexQwordElement) {
    auto cpu = fresh();
    cpu.gpr.set64(static_cast<u8>(RegIndex::RAX), kDataBase);
    const u64 table[4] = {0x1111111111111111ull, 0x2222222222222222ull,
                          0x3333333333333333ull, 0x4444444444444444ull};
    ASSERT_TRUE(m_memory->write_bytes(kDataBase,
                                      reinterpret_cast<const u8*>(table), 32));
    cpu.sse.xmm[1].u64_[0] = 2;
    cpu.sse.xmm[1].u64_[1] = 0;
    cpu.sse.xmm[2].u64_[0] = 0x8000000000000000ull;
    cpu.sse.xmm[2].u64_[1] = 0x8000000000000000ull;
    cpu.avx.ymm_high[0].u64_[0] = 0xEEEEEEEEEEEEEEEEull;

    ASSERT_EQ(run({0xC4, 0xE2, 0xE9, 0x91, 0x04, 0xC8}, cpu),
              ExecResult::Continue);

    EXPECT_EQ(cpu.sse.xmm[0].u64_[0], 0x3333333333333333ull);
    EXPECT_EQ(cpu.sse.xmm[0].u64_[1], 0x1111111111111111ull);
    EXPECT_EQ(cpu.avx.ymm_high[0].u64_[0], 0ull);
    EXPECT_EQ(cpu.sse.xmm[2].u64_[0], 0ull);
    EXPECT_EQ(cpu.sse.xmm[2].u64_[1], 0ull);
}

// VEX.128.66.0F38.W1 92 /r — vgatherdpd xmm0, [rax + xmm1*8], xmm2.
// Dword indices with qword elements: only the low 2 index dwords are read.
TEST_F(AvxPermuteGatherTest, VgatherdpdDwordIndexQwordElement) {
    auto cpu = fresh();
    cpu.gpr.set64(static_cast<u8>(RegIndex::RAX), kDataBase);
    const u64 table[4] = {0x1111111111111111ull, 0x2222222222222222ull,
                          0x3333333333333333ull, 0x4444444444444444ull};
    ASSERT_TRUE(m_memory->write_bytes(kDataBase,
                                      reinterpret_cast<const u8*>(table), 32));
    cpu.sse.xmm[1].u32_[0] = 1;
    cpu.sse.xmm[1].u32_[1] = 3;
    cpu.sse.xmm[2].u64_[0] = 0x8000000000000000ull;
    cpu.sse.xmm[2].u64_[1] = 0x8000000000000000ull;

    ASSERT_EQ(run({0xC4, 0xE2, 0xE9, 0x92, 0x04, 0xC8}, cpu),
              ExecResult::Continue);

    EXPECT_EQ(cpu.sse.xmm[0].u64_[0], 0x2222222222222222ull);
    EXPECT_EQ(cpu.sse.xmm[0].u64_[1], 0x4444444444444444ull);
}

// Intel SDM Vol. 2: #UD if any pair of destination, index, and mask is the
// same register. Here destination and mask are both xmm0.
TEST_F(AvxPermuteGatherTest, GatherFaultsWhenRegistersOverlap) {
    auto cpu = fresh();
    cpu.gpr.set64(static_cast<u8>(RegIndex::RAX), kDataBase);
    // vgatherqps xmm0, [rax + xmm1*4], xmm0  (VEX.vvvv = 0)
    ASSERT_EQ(run({0xC4, 0xE2, 0x79, 0x93, 0x04, 0x48}, cpu),
              ExecResult::Exception);
}

// ===========================================================================
// Decode-only checks for the remaining forms
// ===========================================================================

TEST(AvxPermuteGatherDecode, DistinguishesGatherFormsByOpcodeAndW) {
    // W0 93 -> qword index, dword element (float)
    EXPECT_EQ(decode64({0xC4, 0xE2, 0x69, 0x93, 0x04, 0x48}).opcode,
              Opcode::VGATHERQPS);
    // W0 91 -> qword index, dword element (integer)
    EXPECT_EQ(decode64({0xC4, 0xE2, 0x69, 0x91, 0x04, 0x48}).opcode,
              Opcode::VPGATHERQD);
    // W1 92 -> dword index, qword element (float)
    EXPECT_EQ(decode64({0xC4, 0xE2, 0xE9, 0x92, 0x04, 0xC8}).opcode,
              Opcode::VGATHERDPD);
    // W1 90 -> dword index, qword element (integer)
    EXPECT_EQ(decode64({0xC4, 0xE2, 0xE9, 0x90, 0x04, 0xC8}).opcode,
              Opcode::VPGATHERDQ);
    // W1 93 -> qword index, qword element (float)
    EXPECT_EQ(decode64({0xC4, 0xE2, 0xE9, 0x93, 0x04, 0xC8}).opcode,
              Opcode::VGATHERQPD);
    // W1 91 -> qword index, qword element (integer)
    EXPECT_EQ(decode64({0xC4, 0xE2, 0xE9, 0x91, 0x04, 0xC8}).opcode,
              Opcode::VPGATHERQQ);
    // The pre-existing W0 dword-index forms must still decode as before.
    EXPECT_EQ(decode64({0xC4, 0xE2, 0x69, 0x92, 0x04, 0x48}).opcode,
              Opcode::VGATHERDPS_XMM_VM32X_XMM);
    EXPECT_EQ(decode64({0xC4, 0xE2, 0x69, 0x90, 0x04, 0x48}).opcode,
              Opcode::VPGATHERDD_XMM_VM32X_XMM);
}

TEST(AvxPermuteGatherDecode, MaskmovLoadAndStoreDifferByOpcodeByte) {
    EXPECT_EQ(decode64({0xC4, 0xE2, 0x71, 0x2C, 0x00}).opcode,
              Opcode::VMASKMOVPS_LOAD);
    EXPECT_EQ(decode64({0xC4, 0xE2, 0x71, 0x2D, 0x00}).opcode,
              Opcode::VMASKMOVPD_LOAD);
    EXPECT_EQ(decode64({0xC4, 0xE2, 0x71, 0x2E, 0x00}).opcode,
              Opcode::VMASKMOVPS_STORE);
    EXPECT_EQ(decode64({0xC4, 0xE2, 0x71, 0x2F, 0x00}).opcode,
              Opcode::VMASKMOVPD_STORE);
}

// Intel SDM Vol. 2 VMASKMOV: the r/m operand must be memory.
TEST(AvxPermuteGatherDecode, MaskmovRejectsRegisterOperand) {
    std::vector<u8> bytes{0xC4, 0xE2, 0x71, 0x2C, 0xC1};  // mod=11
    MemoryByteReader reader(bytes.data(), bytes.size());
    DecodedInstruction inst;
    EXPECT_NE(decode_instruction(reader, DecodeMode::Mode64, inst),
              DecodeError::Success);
}
