// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Legacy user-mode ISA: ENDBR, hint NOP, DAA, BOUND, FFREE (Intel SDM Vol. 2)

#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include <gtest/gtest.h>
#include <cstring>
#include <memory>
#include <vector>

using namespace vitreo;
using namespace vitreo::cpu;

static DecodedInstruction decode(const std::initializer_list<u8>& bytes,
                                 DecodeMode mode = DecodeMode::Mode64) {
    std::vector<u8> data(bytes);
    MemoryByteReader reader(data.data(), data.size());
    DecodedInstruction inst;
    EXPECT_EQ(decode_instruction(reader, mode, inst), DecodeError::Success);
    return inst;
}

class LegacyUsermodeIsaTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        ASSERT_TRUE(m_memory->map_region(0x1000, 0x20000, MemoryProtection::ReadWrite));
    }

    ExecResult run_code(const u8* code, usize len, CpuState& cpu) {
        EXPECT_TRUE(m_memory->write_bytes(0x1000, code, len));
        cpu.rip = 0x1000;
        Interpreter interp(cpu, *m_memory);
        auto r = interp.step();
        m_exception = interp.context().exception;
        return r;
    }

    std::unique_ptr<GuestMemory> m_memory;
    CpuException m_exception = CpuException::None;
};

TEST(Decoder, Endbr64) {
    auto inst = decode({0xF3, 0x0F, 0x1E, 0xFA});
    EXPECT_EQ(inst.opcode, Opcode::ENDBR64);
}

TEST(Decoder, Endbr32) {
    auto inst = decode({0xF3, 0x0F, 0x1E, 0xFB});
    EXPECT_EQ(inst.opcode, Opcode::ENDBR32);
}

TEST(Decoder, HintNop0F1D) {
    auto inst = decode({0x0F, 0x1D, 0x00});
    EXPECT_EQ(inst.opcode, Opcode::NOP_RM);
}

TEST(Decoder, DaaIsUdIn64Bit) {
    std::vector<u8> data{0x27};
    MemoryByteReader reader(data.data(), data.size());
    DecodedInstruction inst;
    EXPECT_EQ(decode_instruction(reader, DecodeMode::Mode64, inst),
              DecodeError::InvalidOpcode);
}

TEST(Decoder, DaaIn32Bit) {
    auto inst = decode({0x27}, DecodeMode::Mode32);
    EXPECT_EQ(inst.opcode, Opcode::DAA);
}

TEST(Decoder, FfreeSt0) {
    auto inst = decode({0xDD, 0xC0});
    EXPECT_EQ(inst.opcode, Opcode::FFREE_STI);
}

TEST_F(LegacyUsermodeIsaTest, Endbr64IsNop) {
    CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.set64(static_cast<u8>(RegIndex::RSP), 0x10000);
    const u64 rax = 0x1122334455667788ull;
    cpu.gpr.set64(0, rax);
    const u8 code[] = {0xF3, 0x0F, 0x1E, 0xFA};
    EXPECT_EQ(run_code(code, sizeof(code), cpu), ExecResult::Continue);
    EXPECT_EQ(cpu.gpr.get64(0), rax);
    EXPECT_EQ(cpu.rip, 0x1000u + sizeof(code));
}

TEST_F(LegacyUsermodeIsaTest, DaaAdjustsAl) {
    CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.set32(static_cast<u8>(RegIndex::RSP), 0x10000);
    cpu.gpr.set8l(0, 0x0A);
    cpu.flags.set_af(false);
    cpu.flags.set_cf(false);
    const u8 code[] = {0x27};
    EXPECT_EQ(run_code(code, sizeof(code), cpu), ExecResult::Continue);
    EXPECT_EQ(cpu.gpr.get8l(0), 0x10);
    EXPECT_TRUE(cpu.flags.af());
}

TEST_F(LegacyUsermodeIsaTest, BoundRaisesBrWhenOutOfRange) {
    CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.set32(static_cast<u8>(RegIndex::RSP), 0x10000);
    cpu.gpr.set32(0, 50);  // EAX
    ASSERT_TRUE(m_memory->write_u32(0x2000, 0));   // lower
    ASSERT_TRUE(m_memory->write_u32(0x2004, 10));  // upper
    const u8 code[] = {0x62, 0x05, 0x00, 0x20, 0x00, 0x00};  // BOUND EAX, [0x2000]
    EXPECT_EQ(run_code(code, sizeof(code), cpu), ExecResult::Exception);
    EXPECT_EQ(m_exception, CpuException::BoundRange);
}

TEST_F(LegacyUsermodeIsaTest, FfreeMarksSt0Empty) {
    CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.set64(static_cast<u8>(RegIndex::RSP), 0x10000);
    cpu.fpu.push(Float80(1.0));
    EXPECT_NE(cpu.fpu.get_tag(cpu.fpu.st_index(0)), FpuTag::Empty);
    const u8 code[] = {0xDD, 0xC0};
    EXPECT_EQ(run_code(code, sizeof(code), cpu), ExecResult::Continue);
    EXPECT_EQ(cpu.fpu.get_tag(cpu.fpu.st_index(0)), FpuTag::Empty);
}
