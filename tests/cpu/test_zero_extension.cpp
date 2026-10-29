// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// x86-64 zero-extension audit: every instruction that writes a 32-bit GPR must
// clear bits 63:32 of the full 64-bit register. A single op that fails to
// zero-extend leaves stale high bits, and when the result feeds a pointer the
// dereference faults with a valid-low-32 / garbage-high-32 address — the exact
// signature seen corrupting RBX/R15 in the Steam webhelper (libcef memcpy).

#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include "core/memory/guest_memory.hpp"
#include <gtest/gtest.h>

using namespace vitreo;
using namespace vitreo::cpu;

namespace {

class ZeroExtTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_mem = std::make_unique<GuestMemory>();
        ASSERT_TRUE(m_mem->map_region(0x1000, 0x10000,
                                      MemoryProtection::ReadWrite));
        m_cpu = std::make_unique<CpuState>();
        m_cpu->init_long_mode();
        m_cpu->gpr.set64(static_cast<u8>(RegIndex::RSP), 0x8000);
    }

    // Seed all GPRs with garbage high 32 bits so any failure to zero-extend is
    // visible as a non-zero upper half.
    void poison_high_bits() {
        for (u8 i = 0; i < 16; ++i)
            m_cpu->gpr.set64(i, 0xCAFEF00D00000000ull | m_cpu->gpr.r[i]);
    }

    ExecResult run(std::initializer_list<u8> code) {
        std::vector<u8> bytes(code);
        EXPECT_TRUE(m_mem->write_bytes(0x1000, bytes.data(), bytes.size()));
        m_cpu->rip = 0x1000;
        Interpreter interp(*m_cpu, *m_mem);
        return interp.step();
    }

    u64 reg(RegIndex r) { return m_cpu->gpr.get64(static_cast<u8>(r)); }

    std::unique_ptr<GuestMemory> m_mem;
    std::unique_ptr<CpuState> m_cpu;
};

// Bits 63:32 must be zero after a 32-bit-writing op.
#define EXPECT_ZERO_EXTENDED(r) \
    EXPECT_EQ(reg(r) >> 32, 0u) << #r " upper 32 bits not cleared: 0x" \
        << std::hex << reg(r)

}  // namespace

TEST_F(ZeroExtTest, MovR32R32) {
    // mov r15d, r8d   (45 89 C7)
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::R8), 0x12345678);
    poison_high_bits();
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::R8),
                     0xCAFEF00D12345678ull);  // realistic: r8d valid, high junk
    ASSERT_EQ(run({0x45, 0x89, 0xC7}), ExecResult::Continue);
    EXPECT_EQ(reg(RegIndex::R15) & 0xFFFFFFFF, 0x12345678u);
    EXPECT_ZERO_EXTENDED(RegIndex::R15);
}

TEST_F(ZeroExtTest, AndR32R32) {
    // and r15d, r9d   (45 21 CF)
    poison_high_bits();
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::R15), 0xCAFEF00DFFFFFFFFull);
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::R9),  0xCAFEF00D0000FFFFull);
    ASSERT_EQ(run({0x45, 0x21, 0xCF}), ExecResult::Continue);
    EXPECT_EQ(reg(RegIndex::R15) & 0xFFFFFFFF, 0x0000FFFFu);
    EXPECT_ZERO_EXTENDED(RegIndex::R15);
}

TEST_F(ZeroExtTest, ShlR32Cl) {
    // shl r15d, cl    (41 D3 E7), cl = 4
    poison_high_bits();
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::R15), 0xCAFEF00D0FFFFFFFull);
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RCX), 0x04);
    ASSERT_EQ(run({0x41, 0xD3, 0xE7}), ExecResult::Continue);
    EXPECT_ZERO_EXTENDED(RegIndex::R15);
}

TEST_F(ZeroExtTest, NotR32) {
    // not r15d        (41 F7 D7)
    poison_high_bits();
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::R15), 0xCAFEF00D0000FFFFull);
    ASSERT_EQ(run({0x41, 0xF7, 0xD7}), ExecResult::Continue);
    EXPECT_EQ(reg(RegIndex::R15) & 0xFFFFFFFF, 0xFFFF0000u);
    EXPECT_ZERO_EXTENDED(RegIndex::R15);
}

TEST_F(ZeroExtTest, MovzxR32M8) {
    // movzx r8d, byte [rax]   (44 0F B6 00)
    poison_high_bits();
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RAX), 0x2000);
    ASSERT_TRUE(m_mem->write_u8(0x2000, 0xAB));
    ASSERT_EQ(run({0x44, 0x0F, 0xB6, 0x00}), ExecResult::Continue);
    EXPECT_EQ(reg(RegIndex::R8) & 0xFFFFFFFF, 0xABu);
    EXPECT_ZERO_EXTENDED(RegIndex::R8);
}

TEST_F(ZeroExtTest, AddR32R32) {
    // add r15d, r8d   (45 01 C7)
    poison_high_bits();
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::R15), 0xCAFEF00D00000010ull);
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::R8),  0xCAFEF00D00000020ull);
    ASSERT_EQ(run({0x45, 0x01, 0xC7}), ExecResult::Continue);
    EXPECT_EQ(reg(RegIndex::R15) & 0xFFFFFFFF, 0x30u);
    EXPECT_ZERO_EXTENDED(RegIndex::R15);
}

TEST_F(ZeroExtTest, IncR32) {
    // inc r15d        (41 FF C7)
    poison_high_bits();
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::R15), 0xCAFEF00D024889B3ull);
    ASSERT_EQ(run({0x41, 0xFF, 0xC7}), ExecResult::Continue);
    EXPECT_EQ(reg(RegIndex::R15) & 0xFFFFFFFF, 0x024889B4u);
    EXPECT_ZERO_EXTENDED(RegIndex::R15);
}
