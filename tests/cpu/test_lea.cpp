// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// LEA instruction tests - CPU-018

#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include <gtest/gtest.h>

using namespace vitreo;
using namespace vitreo::cpu;

// Helper to decode instruction from bytes
static DecodedInstruction decode(const std::initializer_list<u8>& bytes, DecodeMode mode = DecodeMode::Mode64) {
    std::vector<u8> data(bytes);
    MemoryByteReader reader(data.data(), data.size());
    DecodedInstruction inst;
    DecodeError err = decode_instruction(reader, mode, inst);
    EXPECT_EQ(err, DecodeError::Success) << "Decode failed";
    return inst;
}

// ============================================================================
// LEA Decoder Tests
// ============================================================================

TEST(LeaDecoder, LeaR64Disp32) {
    // 48 8D 05 10 00 00 00 - LEA RAX, [RIP+0x10]
    auto inst = decode({0x48, 0x8D, 0x05, 0x10, 0x00, 0x00, 0x00});
    EXPECT_EQ(inst.opcode, Opcode::LEA_R64_M);
    EXPECT_EQ(inst.length, 7);
    EXPECT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 0);  // RAX
    EXPECT_TRUE(inst.operands[1].mem.is_rip_relative());
    EXPECT_EQ(inst.operands[1].mem.disp, 0x10);
}

TEST(LeaDecoder, LeaR32BasePlusIndex) {
    // 8D 04 08 - LEA EAX, [EAX+ECX]
    auto inst = decode({0x8D, 0x04, 0x08});
    EXPECT_EQ(inst.opcode, Opcode::LEA_R32_M);
    EXPECT_EQ(inst.length, 3);
    EXPECT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 0);  // EAX
    EXPECT_EQ(inst.operands[1].mem.base, 0);   // EAX
    EXPECT_EQ(inst.operands[1].mem.index, 1);  // ECX
    EXPECT_EQ(inst.operands[1].mem.scale, 1);
    EXPECT_EQ(inst.operands[1].mem.disp, 0);
}

TEST(LeaDecoder, LeaR64WithDisp8) {
    // 48 8D 45 F8 - LEA RAX, [RBP-8]
    auto inst = decode({0x48, 0x8D, 0x45, 0xF8});
    EXPECT_EQ(inst.opcode, Opcode::LEA_R64_M);
    EXPECT_EQ(inst.length, 4);
    EXPECT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 0);  // RAX
    EXPECT_EQ(inst.operands[1].mem.base, 5);  // RBP
    EXPECT_EQ(inst.operands[1].mem.disp, -8);
}

// ============================================================================
// Interpreter Tests
// ============================================================================

class LeaInterpreterTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        auto map_result = m_memory->map_region(0x1000, 0x10000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(map_result);

        m_cpu = std::make_unique<CpuState>();
        m_cpu->init_long_mode();
        m_cpu->gpr.set64(static_cast<u8>(RegIndex::RSP), 0x10000);
        m_cpu->rip = 0x1000;
    }

    void write_code(const u8* code, usize len) {
        auto result = m_memory->write_bytes(0x1000, code, len);
        ASSERT_TRUE(result);
    }

    ExecResult execute_one() {
        Interpreter interp(*m_cpu, *m_memory);
        return interp.step();
    }

    std::unique_ptr<GuestMemory> m_memory;
    std::unique_ptr<CpuState> m_cpu;
};

// ============================================================================
// LEA Interpreter Tests
// ============================================================================

TEST_F(LeaInterpreterTest, LeaSimpleDisplacement) {
    // LEA RAX, [RBP-8]: 48 8D 45 F8
    // RBP=0x5000 → RAX = 0x5000-8 = 0x4FF8
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBP), 0x5000);

    u8 code[] = { 0x48, 0x8D, 0x45, 0xF8 };
    write_code(code, sizeof(code));

    // Save flags to verify LEA does not change them
    bool zf_before = m_cpu->flags.zf();
    bool cf_before = m_cpu->flags.cf();
    bool sf_before = m_cpu->flags.sf();
    bool of_before = m_cpu->flags.of();

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0x4FF8);

    // LEA must not modify flags
    EXPECT_EQ(m_cpu->flags.zf(), zf_before);
    EXPECT_EQ(m_cpu->flags.cf(), cf_before);
    EXPECT_EQ(m_cpu->flags.sf(), sf_before);
    EXPECT_EQ(m_cpu->flags.of(), of_before);
}

TEST_F(LeaInterpreterTest, LeaBaseIndex) {
    // LEA RAX, [RDX+RCX+0x10]: 48 8D 44 0A 10
    // ModR/M=0x44 (mod=01, reg=RAX, rm=100=SIB), SIB=0x0A (scale=1, index=RCX, base=RDX), disp8=0x10
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RDX), 100);
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RCX), 50);

    u8 code[] = { 0x48, 0x8D, 0x44, 0x0A, 0x10 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // RAX = RDX + RCX + 0x10 = 100 + 50 + 16 = 166
    EXPECT_EQ(m_cpu->gpr.get64(0), 166);
}

TEST_F(LeaInterpreterTest, LeaScaledIndex) {
    // LEA RAX, [RCX*2+0x100]: 48 8D 04 4D 00 01 00 00
    // ModR/M=0x04 (mod=00, reg=RAX, rm=100=SIB)
    // SIB=0x4D (scale=01=*2, index=RCX=1, base=RBP=5 → with mod=00 means disp32 no base)
    // disp32=0x00000100
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RCX), 10);

    u8 code[] = { 0x48, 0x8D, 0x04, 0x4D, 0x00, 0x01, 0x00, 0x00 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // RAX = RCX*2 + 0x100 = 10*2 + 256 = 276
    EXPECT_EQ(m_cpu->gpr.get64(0), 276);
}

TEST_F(LeaInterpreterTest, LeaDoesNotReadMemory) {
    // LEA computes an address without accessing memory.
    // Use an address in unmapped region — should NOT fault.
    // LEA RAX, [RBP-8]: 48 8D 45 F8
    // RBP points to unmapped memory
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBP), 0xDEAD0000);

    u8 code[] = { 0x48, 0x8D, 0x45, 0xF8 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0xDEAD0000 - 8);
}

TEST_F(LeaInterpreterTest, LeaR32Truncates) {
    // LEA EAX, [EAX+ECX]: 8D 04 08 (no REX prefix → 32-bit)
    // 32-bit LEA should zero-extend result, clearing upper 32 bits of RAX
    m_cpu->gpr.set64(0, 0xFFFFFFFF00000064ULL);  // RAX upper bits set, lower = 100
    m_cpu->gpr.set64(1, 50);                       // RCX = 50

    u8 code[] = { 0x8D, 0x04, 0x08 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // EAX = lower32(RAX) + lower32(RCX) = 100 + 50 = 150, zero-extended
    EXPECT_EQ(m_cpu->gpr.get64(0), 150);
}
