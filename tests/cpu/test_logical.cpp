// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Logical instruction tests (AND/OR/XOR/NOT) - CPU-016

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
// AND Decoder Tests
// ============================================================================

TEST(LogicalDecoder, AndAlImm8) {
    // 24 0F - AND AL, 0x0F
    auto inst = decode({0x24, 0x0F});
    EXPECT_EQ(inst.opcode, Opcode::AND_AL_IMM8);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[1].type, OperandType::Imm8);
    EXPECT_EQ(inst.operands[1].imm, 0x0F);
}

TEST(LogicalDecoder, AndEaxImm32) {
    // 25 00 00 FF 00 - AND EAX, 0x00FF0000
    auto inst = decode({0x25, 0x00, 0x00, 0xFF, 0x00});
    EXPECT_EQ(inst.opcode, Opcode::AND_EAX_IMM32);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].imm, 0x00FF0000);
}

TEST(LogicalDecoder, AndRaxImm32) {
    // 48 25 FF 00 00 00 - AND RAX, 0xFF
    auto inst = decode({0x48, 0x25, 0xFF, 0x00, 0x00, 0x00});
    EXPECT_EQ(inst.opcode, Opcode::AND_RAX_IMM32);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
}

TEST(LogicalDecoder, AndRm32R32) {
    // 21 D8 - AND EAX, EBX (r/m32, r32)
    auto inst = decode({0x21, 0xD8});
    EXPECT_EQ(inst.opcode, Opcode::AND_RM32_R32);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 0);  // EAX
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].reg, 3);  // EBX
}

TEST(LogicalDecoder, AndR64Rm64) {
    // 48 23 C3 - AND RAX, RBX (r64, r/m64)
    auto inst = decode({0x48, 0x23, 0xC3});
    EXPECT_EQ(inst.opcode, Opcode::AND_R64_RM64);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 0);  // RAX
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[1].reg, 3);  // RBX
}

// ============================================================================
// OR Decoder Tests
// ============================================================================

TEST(LogicalDecoder, OrAlImm8) {
    // 0C 0F - OR AL, 0x0F
    auto inst = decode({0x0C, 0x0F});
    EXPECT_EQ(inst.opcode, Opcode::OR_AL_IMM8);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operands[1].imm, 0x0F);
}

TEST(LogicalDecoder, OrRm32R32) {
    // 09 D8 - OR EAX, EBX (r/m32, r32)
    auto inst = decode({0x09, 0xD8});
    EXPECT_EQ(inst.opcode, Opcode::OR_RM32_R32);
    EXPECT_EQ(inst.operands[0].reg, 0);
    EXPECT_EQ(inst.operands[1].reg, 3);
}

TEST(LogicalDecoder, OrR64Rm64) {
    // 48 0B C3 - OR RAX, RBX (r64, r/m64)
    auto inst = decode({0x48, 0x0B, 0xC3});
    EXPECT_EQ(inst.opcode, Opcode::OR_R64_RM64);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
}

// ============================================================================
// XOR Decoder Tests
// ============================================================================

TEST(LogicalDecoder, XorAlImm8) {
    // 34 FF - XOR AL, 0xFF
    auto inst = decode({0x34, 0xFF});
    EXPECT_EQ(inst.opcode, Opcode::XOR_AL_IMM8);
    EXPECT_EQ(inst.operands[1].imm, static_cast<i64>(0xFF));
}

TEST(LogicalDecoder, XorEaxImm32) {
    // 35 FF FF FF FF - XOR EAX, 0xFFFFFFFF
    auto inst = decode({0x35, 0xFF, 0xFF, 0xFF, 0xFF});
    EXPECT_EQ(inst.opcode, Opcode::XOR_EAX_IMM32);
}

TEST(LogicalDecoder, XorRm32R32) {
    // 31 C0 - XOR EAX, EAX (common zero idiom)
    auto inst = decode({0x31, 0xC0});
    EXPECT_EQ(inst.opcode, Opcode::XOR_RM32_R32);
    EXPECT_EQ(inst.operands[0].reg, 0);  // EAX
    EXPECT_EQ(inst.operands[1].reg, 0);  // EAX
}

TEST(LogicalDecoder, XorR64Rm64) {
    // 48 33 C0 - XOR RAX, RAX (64-bit zero idiom)
    auto inst = decode({0x48, 0x33, 0xC0});
    EXPECT_EQ(inst.opcode, Opcode::XOR_R64_RM64);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
}

// ============================================================================
// NOT Decoder Tests
// ============================================================================

TEST(LogicalDecoder, NotRm32) {
    // F7 D0 - NOT EAX
    auto inst = decode({0xF7, 0xD0});
    EXPECT_EQ(inst.opcode, Opcode::NOT_RM32);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 0);
}

TEST(LogicalDecoder, NotRm64) {
    // 48 F7 D0 - NOT RAX
    auto inst = decode({0x48, 0xF7, 0xD0});
    EXPECT_EQ(inst.opcode, Opcode::NOT_RM64);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 0);
}

TEST(LogicalDecoder, NotRm8) {
    // F6 D3 - NOT BL
    auto inst = decode({0xF6, 0xD3});
    EXPECT_EQ(inst.opcode, Opcode::NOT_RM8);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[0].reg, 3);  // BL
}

// ============================================================================
// Group 1 Immediate Logical (83 /4, /1, /6)
// ============================================================================

TEST(LogicalDecoder, AndRm64Imm8_Group) {
    // 48 83 E0 0F - AND RAX, 0x0F (Group 1, reg=4)
    auto inst = decode({0x48, 0x83, 0xE0, 0x0F});
    EXPECT_EQ(inst.opcode, Opcode::AND_RM64_IMM8);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 0);
    EXPECT_EQ(inst.operands[1].imm, 0x0F);
}

TEST(LogicalDecoder, OrRm64Imm8_Group) {
    // 48 83 C8 0F - OR RAX, 0x0F (Group 1, reg=1)
    auto inst = decode({0x48, 0x83, 0xC8, 0x0F});
    EXPECT_EQ(inst.opcode, Opcode::OR_RM64_IMM8);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
}

TEST(LogicalDecoder, XorRm64Imm8_Group) {
    // 48 83 F0 FF - XOR RAX, -1 (Group 1, reg=6)
    auto inst = decode({0x48, 0x83, 0xF0, 0xFF});
    EXPECT_EQ(inst.opcode, Opcode::XOR_RM64_IMM8);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
}

// ============================================================================
// Interpreter Tests
// ============================================================================

class LogicalInterpreterTest : public ::testing::Test {
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

// --- AND execution tests ---

TEST_F(LogicalInterpreterTest, AndR64R64_Basic) {
    // AND RAX, RBX: 48 21 D8
    m_cpu->gpr.set64(0, 0xFF00FF00FF00FF00ULL);
    m_cpu->gpr.set64(3, 0x00FF00FF00FF00FFULL);

    u8 code[] = { 0x48, 0x21, 0xD8 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0x0000000000000000ULL);
    EXPECT_TRUE(m_cpu->flags.zf());
    EXPECT_FALSE(m_cpu->flags.sf());
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.of());
}

TEST_F(LogicalInterpreterTest, AndClearsCarryAndOverflow) {
    // AND always clears CF and OF
    m_cpu->flags.set_cf(true);
    m_cpu->flags.set_of(true);
    m_cpu->gpr.set64(0, 0xFFFFFFFF);
    m_cpu->gpr.set64(3, 0x0000FFFF);

    u8 code[] = { 0x48, 0x21, 0xD8 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0x0000FFFF);
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.of());
}

TEST_F(LogicalInterpreterTest, AndSetsSignFlag) {
    // AND with result having high bit set
    m_cpu->gpr.set64(0, 0xFFFFFFFFFFFFFFFFULL);
    m_cpu->gpr.set64(3, 0x8000000000000000ULL);

    u8 code[] = { 0x48, 0x21, 0xD8 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_TRUE(m_cpu->flags.sf());
    EXPECT_FALSE(m_cpu->flags.zf());
}

// --- OR execution tests ---

TEST_F(LogicalInterpreterTest, OrR64R64_Basic) {
    // OR RAX, RBX: 48 09 D8
    m_cpu->gpr.set64(0, 0xFF00FF00FF00FF00ULL);
    m_cpu->gpr.set64(3, 0x00FF00FF00FF00FFULL);

    u8 code[] = { 0x48, 0x09, 0xD8 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0xFFFFFFFFFFFFFFFFULL);
    EXPECT_FALSE(m_cpu->flags.zf());
    EXPECT_TRUE(m_cpu->flags.sf());
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.of());
}

TEST_F(LogicalInterpreterTest, OrZeroWithZero) {
    // OR RAX, RBX where both zero
    m_cpu->gpr.set64(0, 0);
    m_cpu->gpr.set64(3, 0);

    u8 code[] = { 0x48, 0x09, 0xD8 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0);
    EXPECT_TRUE(m_cpu->flags.zf());
}

// --- XOR execution tests ---

TEST_F(LogicalInterpreterTest, XorSelfClearsRegister) {
    // XOR EAX, EAX: 31 C0 (common zero idiom)
    m_cpu->gpr.set64(0, 0x12345678DEADBEEFULL);

    u8 code[] = { 0x31, 0xC0 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // XOR EAX, EAX zeroes the full 64-bit register in 64-bit mode
    EXPECT_EQ(m_cpu->gpr.get64(0), 0);
    EXPECT_TRUE(m_cpu->flags.zf());
    EXPECT_FALSE(m_cpu->flags.sf());
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.of());
}

TEST_F(LogicalInterpreterTest, XorR64SelfClearsRegister) {
    // XOR RAX, RAX: 48 31 C0
    m_cpu->gpr.set64(0, 0xDEADBEEFCAFEBABEULL);

    u8 code[] = { 0x48, 0x31, 0xC0 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0);
    EXPECT_TRUE(m_cpu->flags.zf());
}

TEST_F(LogicalInterpreterTest, XorR64R64_Basic) {
    // XOR RAX, RBX: 48 31 D8
    m_cpu->gpr.set64(0, 0xFF00FF00FF00FF00ULL);
    m_cpu->gpr.set64(3, 0xFFFFFFFF00000000ULL);

    u8 code[] = { 0x48, 0x31, 0xD8 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0x00FF00FFFF00FF00ULL);
}

// --- NOT execution tests ---

TEST_F(LogicalInterpreterTest, NotRm64) {
    // NOT RAX: 48 F7 D0
    m_cpu->gpr.set64(0, 0);

    u8 code[] = { 0x48, 0xF7, 0xD0 };
    write_code(code, sizeof(code));

    // Set flags - NOT should NOT modify them
    m_cpu->flags.set_cf(true);
    m_cpu->flags.set_zf(true);

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0xFFFFFFFFFFFFFFFFULL);
    // NOT does not affect flags
    EXPECT_TRUE(m_cpu->flags.cf());
    EXPECT_TRUE(m_cpu->flags.zf());
}

TEST_F(LogicalInterpreterTest, NotRm32) {
    // NOT EAX: F7 D0
    m_cpu->gpr.set64(0, 0x00000000FFFFFFFFULL);

    u8 code[] = { 0xF7, 0xD0 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // 32-bit NOT clears upper 32 bits
    EXPECT_EQ(m_cpu->gpr.get64(0), 0x0000000000000000ULL);
}

TEST_F(LogicalInterpreterTest, NotRm8) {
    // NOT BL: F6 D3
    m_cpu->gpr.set64(3, 0x00000000000000AAULL);

    u8 code[] = { 0xF6, 0xD3 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(3) & 0xFF, 0x55);
}

TEST_F(LogicalInterpreterTest, NotRm64_AllOnes) {
    // NOT RAX where RAX = all ones
    m_cpu->gpr.set64(0, 0xFFFFFFFFFFFFFFFFULL);

    u8 code[] = { 0x48, 0xF7, 0xD0 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0);
}
