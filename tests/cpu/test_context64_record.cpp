// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - x64 CONTEXT record store/load against the Windows measurement in
// tools/oracle_probe/thread_context_probe.expected.

#include "core/cpu/state/context64_record.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include <gtest/gtest.h>

using namespace vitreo;
using namespace vitreo::cpu;

namespace {

constexpr GuestAddr kRecord = 0x10000;

class Context64RecordTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(m_memory.map_region(kRecord, 0x1000, MemoryProtection::ReadWrite).is_ok());
        m_cpu.init_long_mode();
        for (u8 reg = 0; reg < 16; ++reg) {
            m_cpu.gpr.set64(reg, 0xA0A0A0A000000000ull | (u64{reg} << 8) | reg);
        }
        m_cpu.rip = 0x140001000;
        m_cpu.flags.rflags = 0x46;  // ZF PF, as the probe's cmp leaves them
        m_cpu.sse.mxcsr = 0x1FA0;
        for (u32 i = 0; i < 16; ++i) {
            m_cpu.sse.xmm[i].u64_[0] = 0x5A5A000000000000ull | (u64{2 * i} << 16) | (0x100 + 2 * i);
            m_cpu.sse.xmm[i].u64_[1] = 0x5A5A000000000000ull | (u64{2 * i + 1} << 16) | (0x101 + 2 * i);
        }
        m_cpu.fpu.control_word = 0x027F;
        (void)m_cpu.fpu.push(Float80::from_i64(1));
        (void)m_cpu.fpu.push(Float80::from_i64(2));
        fill(0xCC);
    }

    void fill(u8 value) {
        for (u32 off = 0; off < kContext64Size; ++off) {
            ASSERT_TRUE(m_memory.write_u8(kRecord + off, value).is_ok());
        }
    }
    u64 q(u32 off) { return m_memory.read_u64(kRecord + off).value_or(0); }
    u32 d(u32 off) { return m_memory.read_u32(kRecord + off).value_or(0); }
    u16 w(u32 off) { return m_memory.read_u16(kRecord + off).value_or(0); }

    GuestMemory m_memory;
    CpuState m_cpu;
};

constexpr u64 kUntouched = 0xCCCCCCCCCCCCCCCCull;

} // namespace

// GET zero / amd64: CONTEXT_AMD64 is ORed in and nothing else is written.
TEST_F(Context64RecordTest, NoGroupsWritesOnlyTheFlags) {
    ASSERT_EQ(store_context64(m_memory, kRecord, m_cpu, 0), Context64Status::Ok);
    EXPECT_EQ(d(0x30), 0x00100000u);
    EXPECT_EQ(d(0x34), 0xCCCCCCCCu);
    for (u32 off = 0; off < kContext64Size; off += 8) {
        if (off == 0x30) continue;
        EXPECT_EQ(q(off), kUntouched) << "offset 0x" << std::hex << off;
    }
}

// GET control: SegCs, SegSs, EFlags (IF and bit 1 set), Rsp, Rip only.
TEST_F(Context64RecordTest, ControlWritesExactlyItsFields) {
    ASSERT_EQ(store_context64(m_memory, kRecord, m_cpu, 0x00100001), Context64Status::Ok);
    EXPECT_EQ(q(0x38), 0xCCCCCCCCCCCC0033ull);
    EXPECT_EQ(q(0x40), 0x00000246002BCCCCull);
    EXPECT_EQ(q(0x98), m_cpu.gpr.rsp);
    EXPECT_EQ(q(0xF8), 0x140001000ull);
    EXPECT_EQ(q(0x78), kUntouched);  // Rax is INTEGER's
    EXPECT_EQ(q(0x100), kUntouched);
}

// GET integer: fifteen registers, never Rsp.
TEST_F(Context64RecordTest, IntegerSkipsRsp) {
    ASSERT_EQ(store_context64(m_memory, kRecord, m_cpu, 0x00100002), Context64Status::Ok);
    EXPECT_EQ(q(0x78), 0xA0A0A0A000000000ull);
    EXPECT_EQ(q(0x98), kUntouched);
    EXPECT_EQ(q(0xF0), 0xA0A0A0A000000F0Full);
    EXPECT_EQ(q(0x38), kUntouched);
}

// GET segments: Ds/Es/Gs 0x2B, Fs 0x53; Cs/Ss untouched.
TEST_F(Context64RecordTest, SegmentsAreTheUserSelectors) {
    ASSERT_EQ(store_context64(m_memory, kRecord, m_cpu, 0x00100004), Context64Status::Ok);
    EXPECT_EQ(q(0x38), 0x0053002B002BCCCCull);
    EXPECT_EQ(q(0x40), 0xCCCCCCCCCCCC002Bull);
}

// GET float: MxCsr at 0x34, the FXSAVE header and XMM, Reserved4 untouched.
TEST_F(Context64RecordTest, FloatingPointIsTheFxsaveImage) {
    ASSERT_EQ(store_context64(m_memory, kRecord, m_cpu, 0x00100008), Context64Status::Ok);
    EXPECT_EQ(d(0x34), 0x1FA0u);
    EXPECT_EQ(w(0x100), 0x027Fu);                  // ControlWord
    EXPECT_EQ(w(0x102), 0x3000u);                  // StatusWord: TOP = 6
    EXPECT_EQ(m_memory.read_u8(kRecord + 0x104).value_or(0), 0xC0u);  // abridged tags
    EXPECT_EQ(q(0x118), 0x0000FFFF00001FA0ull);    // MxCsr, MxCsr_Mask
    EXPECT_EQ(q(0x1A0), 0x5A5A000000000100ull);    // Xmm0 low
    EXPECT_EQ(q(0x298), 0x5A5A0000001F011Full);    // Xmm15 high
    EXPECT_EQ(q(0x2A0), kUntouched);               // Reserved4
    EXPECT_EQ(q(0x78), kUntouched);
}

// GET debug: Dr0-3/6/7 and the four LastBranch/LastException fields.
TEST_F(Context64RecordTest, DebugRegistersAreZero) {
    ASSERT_EQ(store_context64(m_memory, kRecord, m_cpu, 0x00100010), Context64Status::Ok);
    for (u32 off = 0x48; off <= 0x70; off += 8) EXPECT_EQ(q(off), 0u);
    for (u32 off = 0x4B0; off <= 0x4C8; off += 8) EXPECT_EQ(q(off), 0u);
    EXPECT_EQ(q(0x4A8), kUntouched);
    EXPECT_EQ(q(0x78), kUntouched);
}

// SET control|float: integer fields in the record are NOT applied; MXCSR comes
// from 0x34 (not FltSave.MxCsr); the x87 control word from FltSave.
TEST_F(Context64RecordTest, LoadAppliesOnlySelectedGroups) {
    ASSERT_EQ(store_context64(m_memory, kRecord, m_cpu, kContext64Full), Context64Status::Ok);
    for (u32 off = 0x78; off <= 0xF0; off += 8) {
        if (off != 0x98) ASSERT_TRUE(m_memory.write_u64(kRecord + off, 0).is_ok());
    }
    ASSERT_TRUE(m_memory.write_u64(kRecord + 0xF8, 0x140002000).is_ok());
    ASSERT_TRUE(m_memory.write_u64(kRecord + 0x1A0 + 5 * 16, 0x1122334455667788ull).is_ok());
    ASSERT_TRUE(m_memory.write_u32(kRecord + 0x34, 0x1FC0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(kRecord + 0x118, 0x9F80).is_ok());
    ASSERT_TRUE(m_memory.write_u16(kRecord + 0x100, 0x037F).is_ok());
    ASSERT_TRUE(m_memory.write_u32(kRecord + 0x30, 0x00100009).is_ok());

    CpuState target = m_cpu;
    ASSERT_EQ(load_context64(m_memory, kRecord, target, false), Context64Status::Ok);
    EXPECT_EQ(target.rip, 0x140002000ull);
    EXPECT_EQ(target.gpr.rax, 0xA0A0A0A000000000ull);  // not applied
    EXPECT_EQ(target.gpr.r15, 0xA0A0A0A000000F0Full);
    EXPECT_EQ(target.sse.xmm[5].u64_[0], 0x1122334455667788ull);
    EXPECT_EQ(target.sse.xmm[5].u64_[1], m_cpu.sse.xmm[5].u64_[1]);
    EXPECT_EQ(target.sse.mxcsr, 0x1FC0u);
    EXPECT_EQ(target.fpu.control_word, 0x037Fu);
}

// SET noamd64: a record without CONTEXT_AMD64 is accepted as if it had it.
TEST_F(Context64RecordTest, LoadImpliesAmd64) {
    ASSERT_EQ(store_context64(m_memory, kRecord, m_cpu, kContext64Full), Context64Status::Ok);
    ASSERT_TRUE(m_memory.write_u64(kRecord + 0xF8, 0x140003000).is_ok());
    ASSERT_TRUE(m_memory.write_u32(kRecord + 0x30, 0x00000001).is_ok());
    CpuState target = m_cpu;
    ASSERT_EQ(load_context64(m_memory, kRecord, target, false), Context64Status::Ok);
    EXPECT_EQ(target.rip, 0x140003000ull);
}

// A store followed by a load of every group is the identity on the state the
// record carries, including the x87 stack.
TEST_F(Context64RecordTest, RoundTripPreservesX87Stack) {
    ASSERT_EQ(store_context64(m_memory, kRecord, m_cpu, kContext64Full), Context64Status::Ok);
    CpuState target{};
    target.init_long_mode();
    ASSERT_EQ(load_context64(m_memory, kRecord, target, false), Context64Status::Ok);
    EXPECT_EQ(target.fpu.status_word, m_cpu.fpu.status_word);
    for (u8 i = 0; i < 2; ++i) {
        EXPECT_EQ(target.fpu.get_st(i).mantissa, m_cpu.fpu.get_st(i).mantissa);
        EXPECT_EQ(target.fpu.get_st(i).exponent_sign, m_cpu.fpu.get_st(i).exponent_sign);
    }
    EXPECT_EQ(target.gpr.rbx, m_cpu.gpr.rbx);
    EXPECT_EQ(target.gpr.rsp, m_cpu.gpr.rsp);
}

// Arming a hardware breakpoint is reported, never dropped.
TEST_F(Context64RecordTest, ArmedDebugRegistersAreUnsupported) {
    ASSERT_EQ(store_context64(m_memory, kRecord, m_cpu, 0x0010001F), Context64Status::Ok);
    CpuState target = m_cpu;
    EXPECT_EQ(load_context64(m_memory, kRecord, target, false), Context64Status::Ok);
    ASSERT_TRUE(m_memory.write_u64(kRecord + 0x70, 0x1).is_ok());  // DR7.L0
    EXPECT_EQ(load_context64(m_memory, kRecord, target, false),
              Context64Status::DebugRegistersUnsupported);
}
