// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// VEX.256 VPMOVSX / VPMOVZX: the widening forms take ONE source that is still
// at most 128 bits, and the high destination lane comes from the UPPER HALF OF
// THAT SOURCE -- not from the source register's YMM[255:128], and not from
// +16 bytes of a memory source.
//
// Contract source: Intel SDM Vol. 2, VPMOVZX / VPMOVSX. VPMOVZXBW ymm1,
// xmm2/m128:
//   Packed_Zero_Extend_BYTE_to_WORD(DEST[127:0],   SRC[63:0])
//   Packed_Zero_Extend_BYTE_to_WORD(DEST[255:128], SRC[127:64])

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

class Avx256WideningTest : public ::testing::Test {
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

// Bytes 0..15 = 0x10..0x1F in the low lane; a distinct pattern parked in the
// source's YMM high lane, which the architecture must never read.
void seed_source(CpuState& cpu, u8 reg) {
    for (u8 i = 0; i < 16; ++i) cpu.sse.xmm[reg].u8_[i] = static_cast<u8>(0x10 + i);
    for (u8 i = 0; i < 16; ++i) cpu.avx.ymm_high[reg].u8_[i] = 0xEE;
}

}  // namespace

// VEX.256.66.0F38.WIG 30 /r — vpmovzxbw ymm0, xmm1
TEST_F(Avx256WideningTest, VpmovzxbwYmmRegHighLaneComesFromSourceUpperHalf) {
    auto cpu = fresh();
    seed_source(cpu, 1);

    ASSERT_EQ(run({0xC4, 0xE2, 0x7D, 0x30, 0xC1}, cpu), ExecResult::Continue);

    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(cpu.sse.xmm[0].u16_[i], static_cast<u16>(0x10 + i)) << "low " << i;
        EXPECT_EQ(cpu.avx.ymm_high[0].u16_[i], static_cast<u16>(0x18 + i))
            << "high " << i;
    }
}

// The destination naming the source register too: vpmovzxbw ymm1, xmm1.
TEST_F(Avx256WideningTest, VpmovzxbwYmmDestEqualsSource) {
    auto cpu = fresh();
    seed_source(cpu, 1);

    ASSERT_EQ(run({0xC4, 0xE2, 0x7D, 0x30, 0xC9}, cpu), ExecResult::Continue);

    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(cpu.sse.xmm[1].u16_[i], static_cast<u16>(0x10 + i)) << "low " << i;
        EXPECT_EQ(cpu.avx.ymm_high[1].u16_[i], static_cast<u16>(0x18 + i))
            << "high " << i;
    }
}

// Memory source: the high lane reads SRC[127:64], i.e. +8 bytes, not +16.
TEST_F(Avx256WideningTest, VpmovzxbwYmmMemoryHighLaneReadsPlusEight) {
    auto cpu = fresh();
    u8 data[32];
    for (u8 i = 0; i < 32; ++i) data[i] = static_cast<u8>(0x10 + i);
    ASSERT_TRUE(m_memory->write_bytes(kDataBase, data, sizeof(data)));
    cpu.gpr.set64(static_cast<u8>(RegIndex::RDI), kDataBase);

    // vpmovzxbw ymm0, [rdi]
    ASSERT_EQ(run({0xC4, 0xE2, 0x7D, 0x30, 0x07}, cpu), ExecResult::Continue);

    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(cpu.sse.xmm[0].u16_[i], static_cast<u16>(0x10 + i)) << "low " << i;
        EXPECT_EQ(cpu.avx.ymm_high[0].u16_[i], static_cast<u16>(0x18 + i))
            << "high " << i;
    }
}

// VEX.256.66.0F38.WIG 20 /r — vpmovsxbw ymm0, xmm1, negative bytes sign-extend.
TEST_F(Avx256WideningTest, VpmovsxbwYmmSignExtendsBothLanes) {
    auto cpu = fresh();
    for (u8 i = 0; i < 16; ++i) cpu.sse.xmm[1].u8_[i] = 0x80;
    for (u8 i = 0; i < 16; ++i) cpu.avx.ymm_high[1].u8_[i] = 0x01;

    ASSERT_EQ(run({0xC4, 0xE2, 0x7D, 0x20, 0xC1}, cpu), ExecResult::Continue);

    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(cpu.sse.xmm[0].u16_[i], 0xFF80u) << "low " << i;
        EXPECT_EQ(cpu.avx.ymm_high[0].u16_[i], 0xFF80u) << "high " << i;
    }
}

// VEX.256.66.0F38.WIG 33 /r — vpmovzxwd ymm0, xmm1 (m128 source, half = 8).
TEST_F(Avx256WideningTest, VpmovzxwdYmmHighLaneComesFromSourceUpperHalf) {
    auto cpu = fresh();
    for (int i = 0; i < 8; ++i) cpu.sse.xmm[1].u16_[i] = static_cast<u16>(0x1000 + i);
    for (int i = 0; i < 8; ++i) cpu.avx.ymm_high[1].u16_[i] = 0xEEEE;

    ASSERT_EQ(run({0xC4, 0xE2, 0x7D, 0x33, 0xC1}, cpu), ExecResult::Continue);

    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(cpu.sse.xmm[0].u32_[i], static_cast<u32>(0x1000 + i)) << "low " << i;
        EXPECT_EQ(cpu.avx.ymm_high[0].u32_[i], static_cast<u32>(0x1004 + i))
            << "high " << i;
    }
}

// VEX.256.66.0F38.WIG 31 /r — vpmovzxbd ymm0, xmm1 (m64 source, half = 4).
TEST_F(Avx256WideningTest, VpmovzxbdYmmHighLaneComesFromPlusFour) {
    auto cpu = fresh();
    seed_source(cpu, 1);

    ASSERT_EQ(run({0xC4, 0xE2, 0x7D, 0x31, 0xC1}, cpu), ExecResult::Continue);

    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(cpu.sse.xmm[0].u32_[i], static_cast<u32>(0x10 + i)) << "low " << i;
        EXPECT_EQ(cpu.avx.ymm_high[0].u32_[i], static_cast<u32>(0x14 + i))
            << "high " << i;
    }
}

// VEX.256.66.0F38.WIG 35 /r — vpmovzxdq ymm0, xmm1 (m128 source, half = 8).
TEST_F(Avx256WideningTest, VpmovzxdqYmmHighLaneComesFromSourceUpperHalf) {
    auto cpu = fresh();
    for (int i = 0; i < 4; ++i) cpu.sse.xmm[1].u32_[i] = static_cast<u32>(0xA0 + i);
    for (int i = 0; i < 4; ++i) cpu.avx.ymm_high[1].u32_[i] = 0xEEEEEEEEu;

    ASSERT_EQ(run({0xC4, 0xE2, 0x7D, 0x35, 0xC1}, cpu), ExecResult::Continue);

    for (int i = 0; i < 2; ++i) {
        EXPECT_EQ(cpu.sse.xmm[0].u64_[i], static_cast<u64>(0xA0 + i)) << "low " << i;
        EXPECT_EQ(cpu.avx.ymm_high[0].u64_[i], static_cast<u64>(0xA2 + i))
            << "high " << i;
    }
}
