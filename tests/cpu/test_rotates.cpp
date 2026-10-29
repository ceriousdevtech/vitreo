// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Rotate instruction tests (ROL, ROR, RCL, RCR) - CPU-023

#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include <gtest/gtest.h>

using namespace vitreo;
using namespace vitreo::cpu;

// ============================================================================
// Test Fixture
// ============================================================================

class RotatesTest : public ::testing::Test {
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
// Decoder Tests
// ============================================================================

// Helper to decode instruction from bytes
static DecodedInstruction decode(const std::initializer_list<u8>& bytes, DecodeMode mode = DecodeMode::Mode64) {
    std::vector<u8> data(bytes);
    MemoryByteReader reader(data.data(), data.size());
    DecodedInstruction inst;
    DecodeError err = decode_instruction(reader, mode, inst);
    EXPECT_EQ(err, DecodeError::Success) << "Decode failed";
    return inst;
}

TEST(RotateDecoder, RolRm32By1) {
    // D1 C0 - ROL EAX, 1
    auto inst = decode({0xD1, 0xC0});
    EXPECT_EQ(inst.opcode, Opcode::ROL_RM32_1);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 0);  // EAX
    EXPECT_EQ(inst.operands[1].type, OperandType::Imm8);
    EXPECT_EQ(inst.operands[1].imm, 1);
}

TEST(RotateDecoder, RolRm32ByCl) {
    // D3 C0 - ROL EAX, CL
    auto inst = decode({0xD3, 0xC0});
    EXPECT_EQ(inst.opcode, Opcode::ROL_RM32_CL);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[1].reg, 1);  // CL
}

TEST(RotateDecoder, RolRm32Imm8) {
    // C1 C0 04 - ROL EAX, 4
    auto inst = decode({0xC1, 0xC0, 0x04});
    EXPECT_EQ(inst.opcode, Opcode::ROL_RM32_IMM8);
    EXPECT_EQ(inst.length, 3);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].type, OperandType::Imm8);
    EXPECT_EQ(inst.operands[1].imm, 4);
}

TEST(RotateDecoder, RorRm32By1) {
    // D1 C8 - ROR EAX, 1
    auto inst = decode({0xD1, 0xC8});
    EXPECT_EQ(inst.opcode, Opcode::ROR_RM32_1);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 0);
    EXPECT_EQ(inst.operands[1].imm, 1);
}

TEST(RotateDecoder, RclRm32By1) {
    // D1 D0 - RCL EAX, 1
    auto inst = decode({0xD1, 0xD0});
    EXPECT_EQ(inst.opcode, Opcode::RCL_RM32_1);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].imm, 1);
}

TEST(RotateDecoder, RcrRm32By1) {
    // D1 D8 - RCR EAX, 1
    auto inst = decode({0xD1, 0xD8});
    EXPECT_EQ(inst.opcode, Opcode::RCR_RM32_1);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].imm, 1);
}

TEST(RotateDecoder, RolRm64By1) {
    // 48 D1 C0 - ROL RAX, 1
    auto inst = decode({0x48, 0xD1, 0xC0});
    EXPECT_EQ(inst.opcode, Opcode::ROL_RM64_1);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[1].imm, 1);
}

TEST(RotateDecoder, RolRm8By1) {
    // D0 C0 - ROL AL, 1
    auto inst = decode({0xD0, 0xC0});
    EXPECT_EQ(inst.opcode, Opcode::ROL_RM8_1);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[1].imm, 1);
}

// ============================================================================
// ROL Execution Tests
// ============================================================================

TEST_F(RotatesTest, RolEax1) {
    // D1 C0 = ROL EAX, 1
    // EAX = 0x00000001 → rotate left 1 → 0x00000002
    m_cpu->gpr.set64(0, 0x00000001);
    u8 code[] = { 0xD1, 0xC0 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0x00000002u);
    EXPECT_FALSE(m_cpu->flags.cf());  // bit 0 of result = 0
}

TEST_F(RotatesTest, RolEaxCl) {
    // D3 C0 = ROL EAX, CL
    // EAX = 0x00000001, CL = 4 → 0x00000010
    m_cpu->gpr.set64(0, 0x00000001);
    m_cpu->gpr.set64(1, 0x04);  // ECX, CL = 4
    u8 code[] = { 0xD3, 0xC0 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0x00000010u);
    EXPECT_FALSE(m_cpu->flags.cf());
}

TEST_F(RotatesTest, RolEaxImm8) {
    // C1 C0 04 = ROL EAX, 4
    // EAX = 0x12345678 → rotate left by 4 → 0x23456781
    m_cpu->gpr.set64(0, 0x12345678);
    u8 code[] = { 0xC1, 0xC0, 0x04 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0x23456781u);
    // CF = bit 0 of result = 1
    EXPECT_TRUE(m_cpu->flags.cf());
}

TEST_F(RotatesTest, RolWrapsHighBitToLow) {
    // D1 C0 = ROL EAX, 1
    // EAX = 0x80000000 → ROL 1 → 0x00000001, CF=1
    m_cpu->gpr.set64(0, 0x80000000);
    u8 code[] = { 0xD1, 0xC0 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0x00000001u);
    EXPECT_TRUE(m_cpu->flags.cf());  // LSB of result = 1
    // OF: MSB(result)=0 XOR CF=1 → OF=1
    EXPECT_TRUE(m_cpu->flags.of());
}

// ============================================================================
// ROR Execution Tests
// ============================================================================

TEST_F(RotatesTest, RorEax1) {
    // D1 C8 = ROR EAX, 1
    // EAX = 0x00000002 → rotate right 1 → 0x00000001
    m_cpu->gpr.set64(0, 0x00000002);
    u8 code[] = { 0xD1, 0xC8 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0x00000001u);
    EXPECT_FALSE(m_cpu->flags.cf());  // MSB of result = 0
}

TEST_F(RotatesTest, RorWrapsLowBitToHigh) {
    // D1 C8 = ROR EAX, 1
    // EAX = 0x00000001 → ROR 1 → 0x80000000, CF=1 (MSB of result)
    m_cpu->gpr.set64(0, 0x00000001);
    u8 code[] = { 0xD1, 0xC8 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0x80000000u);
    EXPECT_TRUE(m_cpu->flags.cf());  // MSB of result = 1
    // OF: MSB XOR (MSB-1) = 1 XOR 0 = 1
    EXPECT_TRUE(m_cpu->flags.of());
}

// ============================================================================
// RCL Execution Tests
// ============================================================================

TEST_F(RotatesTest, RclIncludesCarry) {
    // D1 D0 = RCL EAX, 1
    // EAX = 0x00000001, CF=1 → RCL 1 → result = (0x00000001 << 1) | CF = 0x00000003
    // New CF = old bit 31 of original = 0
    m_cpu->gpr.set64(0, 0x00000001);
    m_cpu->flags.set_cf(true);
    u8 code[] = { 0xD1, 0xD0 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0x00000003u);
    EXPECT_FALSE(m_cpu->flags.cf());
}

TEST_F(RotatesTest, RclCarryIntoMsb) {
    // D1 D0 = RCL EAX, 1
    // EAX = 0x80000000, CF=0 → shift left 1 → bit 31 goes to CF, CF(0) goes to bit 0
    // result = 0x00000000, CF = 1
    m_cpu->gpr.set64(0, 0x80000000);
    m_cpu->flags.set_cf(false);
    u8 code[] = { 0xD1, 0xD0 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0x00000000u);
    EXPECT_TRUE(m_cpu->flags.cf());
}

// ============================================================================
// RCR Execution Tests
// ============================================================================

TEST_F(RotatesTest, RcrIncludesCarry) {
    // D1 D8 = RCR EAX, 1
    // EAX = 0x00000002, CF=1 → RCR 1
    // Low bit (0) goes to CF, old CF (1) goes to MSB
    // result = (0x00000002 >> 1) | (1 << 31) = 0x80000001, CF = 0
    m_cpu->gpr.set64(0, 0x00000002);
    m_cpu->flags.set_cf(true);
    u8 code[] = { 0xD1, 0xD8 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0x80000001u);
    EXPECT_FALSE(m_cpu->flags.cf());
}

TEST_F(RotatesTest, RcrLowBitToCarry) {
    // D1 D8 = RCR EAX, 1
    // EAX = 0x00000001, CF=0 → RCR 1
    // Low bit (1) goes to CF, old CF (0) goes to MSB
    // result = 0x00000000, CF = 1
    m_cpu->gpr.set64(0, 0x00000001);
    m_cpu->flags.set_cf(false);
    u8 code[] = { 0xD1, 0xD8 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0x00000000u);
    EXPECT_TRUE(m_cpu->flags.cf());
}

// ============================================================================
// Zero Count Tests
// ============================================================================

TEST_F(RotatesTest, ZeroCountNoChange) {
    // C1 C0 00 = ROL EAX, 0
    // Should not modify value or flags
    m_cpu->gpr.set64(0, 0xDEADBEEF);
    m_cpu->flags.set_cf(true);
    m_cpu->flags.set_of(true);
    u8 code[] = { 0xC1, 0xC0, 0x00 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0xDEADBEEFu);
    EXPECT_TRUE(m_cpu->flags.cf());   // Unchanged
    EXPECT_TRUE(m_cpu->flags.of());   // Unchanged
}

// ============================================================================
// 64-bit Rotate Tests
// ============================================================================

TEST_F(RotatesTest, RolRax1_64bit) {
    // 48 D1 C0 = ROL RAX, 1
    // RAX = 0x8000000000000000 → 0x0000000000000001, CF=1
    m_cpu->gpr.set64(0, 0x8000000000000000ULL);
    u8 code[] = { 0x48, 0xD1, 0xC0 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0x0000000000000001ULL);
    EXPECT_TRUE(m_cpu->flags.cf());
}

TEST_F(RotatesTest, RorRax1_64bit) {
    // 48 D1 C8 = ROR RAX, 1
    // RAX = 0x0000000000000001 → 0x8000000000000000, CF=1 (MSB of result)
    m_cpu->gpr.set64(0, 0x0000000000000001ULL);
    u8 code[] = { 0x48, 0xD1, 0xC8 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0x8000000000000000ULL);
    EXPECT_TRUE(m_cpu->flags.cf());
}

// ============================================================================
// 8-bit Rotate Tests
// ============================================================================

TEST_F(RotatesTest, RolAl1_8bit) {
    // D0 C0 = ROL AL, 1
    // AL = 0x81 (10000001) → rotate left 1 → 0x03 (00000011), CF = 1
    m_cpu->gpr.set64(0, 0x81);
    u8 code[] = { 0xD0, 0xC0 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0) & 0xFF, 0x03u);
    EXPECT_TRUE(m_cpu->flags.cf());  // bit 0 of result = 1
}

TEST_F(RotatesTest, RorAl1_8bit) {
    // D0 C8 = ROR AL, 1
    // AL = 0x81 (10000001) → rotate right 1 → 0xC0 (11000000), CF = 1 (MSB of 8-bit result)
    m_cpu->gpr.set64(0, 0x81);
    u8 code[] = { 0xD0, 0xC8 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0) & 0xFF, 0xC0u);
    EXPECT_TRUE(m_cpu->flags.cf());  // MSB of 8-bit result = 1
}
