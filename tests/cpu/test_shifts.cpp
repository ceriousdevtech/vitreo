// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Shift instruction tests (SHL/SHR/SAR) - CPU-017

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
// SHL Decoder Tests
// ============================================================================

TEST(ShiftDecoder, ShlRm32Imm8) {
    // C1 E0 04 - SHL EAX, 4
    auto inst = decode({0xC1, 0xE0, 0x04});
    EXPECT_EQ(inst.opcode, Opcode::SHL_RM32_IMM8);
    EXPECT_EQ(inst.length, 3);
    EXPECT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 0);  // EAX
    EXPECT_EQ(inst.operands[1].type, OperandType::Imm8);
    EXPECT_EQ(inst.operands[1].imm, 4);
}

TEST(ShiftDecoder, ShlRm64Imm8) {
    // 48 C1 E0 04 - SHL RAX, 4
    auto inst = decode({0x48, 0xC1, 0xE0, 0x04});
    EXPECT_EQ(inst.opcode, Opcode::SHL_RM64_IMM8);
    EXPECT_EQ(inst.length, 4);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 0);  // RAX
    EXPECT_EQ(inst.operands[1].imm, 4);
}

TEST(ShiftDecoder, ShlRm8Imm8) {
    // C0 E0 04 - SHL AL, 4
    auto inst = decode({0xC0, 0xE0, 0x04});
    EXPECT_EQ(inst.opcode, Opcode::SHL_RM8_IMM8);
    EXPECT_EQ(inst.length, 3);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[0].reg, 0);  // AL
    EXPECT_EQ(inst.operands[1].imm, 4);
}

TEST(ShiftDecoder, LegacySlash6DecodesAsShlForEveryOperandWidth) {
    // The canonical SHL encoding uses Group-2 /4. Real x86 processors also
    // accept the legacy /6 alias, including every operand width/count source.
    auto byte_imm = decode({0xC0, 0xF7, 0x00}); // SHL BH, 0
    EXPECT_EQ(byte_imm.opcode, Opcode::SHL_RM8_IMM8);
    EXPECT_EQ(byte_imm.length, 3);
    EXPECT_EQ(byte_imm.operands[0].type, OperandType::Reg8);
    EXPECT_EQ(byte_imm.operands[0].reg, 7); // BH without a REX prefix
    EXPECT_EQ(byte_imm.operands[1].imm, 0);

    auto word_imm = decode({0x66, 0xC1, 0xF0, 0x03}); // SHL AX, 3
    EXPECT_EQ(word_imm.opcode, Opcode::SHL_RM16_IMM8);
    EXPECT_EQ(word_imm.operands[0].type, OperandType::Reg16);

    auto dword_one = decode({0xD1, 0xF0}); // SHL EAX, 1
    EXPECT_EQ(dword_one.opcode, Opcode::SHL_RM32_1);
    EXPECT_EQ(dword_one.operands[0].type, OperandType::Reg32);

    auto qword_cl = decode({0x48, 0xD3, 0xF0}); // SHL RAX, CL
    EXPECT_EQ(qword_cl.opcode, Opcode::SHL_RM64_CL);
    EXPECT_EQ(qword_cl.operands[0].type, OperandType::Reg64);
}

TEST(ShiftDecoder, ShlRm32By1) {
    // D1 E0 - SHL EAX, 1
    auto inst = decode({0xD1, 0xE0});
    EXPECT_EQ(inst.opcode, Opcode::SHL_RM32_1);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 0);
    EXPECT_EQ(inst.operands[1].type, OperandType::Imm8);
    EXPECT_EQ(inst.operands[1].imm, 1);
}

TEST(ShiftDecoder, ShlRm64By1) {
    // 48 D1 E0 - SHL RAX, 1
    auto inst = decode({0x48, 0xD1, 0xE0});
    EXPECT_EQ(inst.opcode, Opcode::SHL_RM64_1);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[1].imm, 1);
}

TEST(ShiftDecoder, ShlRm32ByCl) {
    // D3 E0 - SHL EAX, CL
    auto inst = decode({0xD3, 0xE0});
    EXPECT_EQ(inst.opcode, Opcode::SHL_RM32_CL);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 0);
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[1].reg, 1);  // CL
}

TEST(ShiftDecoder, ShlRm8By1) {
    // D0 E0 - SHL AL, 1
    auto inst = decode({0xD0, 0xE0});
    EXPECT_EQ(inst.opcode, Opcode::SHL_RM8_1);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[1].imm, 1);
}

TEST(ShiftDecoder, ShlRm8ByCl) {
    // D2 E0 - SHL AL, CL
    auto inst = decode({0xD2, 0xE0});
    EXPECT_EQ(inst.opcode, Opcode::SHL_RM8_CL);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[1].reg, 1);  // CL
}

// ============================================================================
// SHR Decoder Tests
// ============================================================================

TEST(ShiftDecoder, ShrRm32Imm8) {
    // C1 E8 04 - SHR EAX, 4
    auto inst = decode({0xC1, 0xE8, 0x04});
    EXPECT_EQ(inst.opcode, Opcode::SHR_RM32_IMM8);
    EXPECT_EQ(inst.length, 3);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 0);
    EXPECT_EQ(inst.operands[1].imm, 4);
}

TEST(ShiftDecoder, ShrRm64Imm8) {
    // 48 C1 E8 04 - SHR RAX, 4
    auto inst = decode({0x48, 0xC1, 0xE8, 0x04});
    EXPECT_EQ(inst.opcode, Opcode::SHR_RM64_IMM8);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
}

TEST(ShiftDecoder, ShrRm32By1) {
    // D1 E8 - SHR EAX, 1
    auto inst = decode({0xD1, 0xE8});
    EXPECT_EQ(inst.opcode, Opcode::SHR_RM32_1);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].imm, 1);
}

TEST(ShiftDecoder, ShrRm32ByCl) {
    // D3 E8 - SHR EAX, CL
    auto inst = decode({0xD3, 0xE8});
    EXPECT_EQ(inst.opcode, Opcode::SHR_RM32_CL);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].reg, 1);  // CL
}

TEST(ShiftDecoder, ShrRm8Imm8) {
    // C0 E8 02 - SHR AL, 2
    auto inst = decode({0xC0, 0xE8, 0x02});
    EXPECT_EQ(inst.opcode, Opcode::SHR_RM8_IMM8);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[1].imm, 2);
}

// ============================================================================
// SAR Decoder Tests
// ============================================================================

TEST(ShiftDecoder, SarRm32Imm8) {
    // C1 F8 04 - SAR EAX, 4
    auto inst = decode({0xC1, 0xF8, 0x04});
    EXPECT_EQ(inst.opcode, Opcode::SAR_RM32_IMM8);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 0);
    EXPECT_EQ(inst.operands[1].imm, 4);
}

TEST(ShiftDecoder, SarRm64Imm8) {
    // 48 C1 F8 04 - SAR RAX, 4
    auto inst = decode({0x48, 0xC1, 0xF8, 0x04});
    EXPECT_EQ(inst.opcode, Opcode::SAR_RM64_IMM8);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[1].imm, 4);
}

TEST(ShiftDecoder, SarRm32By1) {
    // D1 F8 - SAR EAX, 1
    auto inst = decode({0xD1, 0xF8});
    EXPECT_EQ(inst.opcode, Opcode::SAR_RM32_1);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].imm, 1);
}

TEST(ShiftDecoder, SarRm32ByCl) {
    // D3 F8 - SAR EAX, CL
    auto inst = decode({0xD3, 0xF8});
    EXPECT_EQ(inst.opcode, Opcode::SAR_RM32_CL);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].reg, 1);  // CL
}

TEST(ShiftDecoder, SarRm8Imm8) {
    // C0 F8 03 - SAR AL, 3
    auto inst = decode({0xC0, 0xF8, 0x03});
    EXPECT_EQ(inst.opcode, Opcode::SAR_RM8_IMM8);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[1].imm, 3);
}

TEST(ShiftDecoder, ShlRm32Imm8_Ebx) {
    // C1 E3 08 - SHL EBX, 8
    auto inst = decode({0xC1, 0xE3, 0x08});
    EXPECT_EQ(inst.opcode, Opcode::SHL_RM32_IMM8);
    EXPECT_EQ(inst.operands[0].reg, 3);  // EBX
    EXPECT_EQ(inst.operands[1].imm, 8);
}

// ============================================================================
// Interpreter Tests
// ============================================================================

class ShiftInterpreterTest : public ::testing::Test {
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

// --- SHL execution tests ---

TEST_F(ShiftInterpreterTest, ShlRax4) {
    // 48 C1 E0 04 = SHL RAX, 4
    m_cpu->gpr.set64(0, 0x1);
    u8 code[] = { 0x48, 0xC1, 0xE0, 0x04 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0x10);
}

TEST_F(ShiftInterpreterTest, ShlEax4) {
    // C1 E0 04 = SHL EAX, 4
    m_cpu->gpr.set64(0, 0xFF);
    u8 code[] = { 0xC1, 0xE0, 0x04 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0xFF0);
}

TEST_F(ShiftInterpreterTest, ShlSetsCF) {
    // SHL RAX, 1 where RAX has MSB set → CF should be set
    // 48 D1 E0 = SHL RAX, 1
    m_cpu->gpr.set64(0, 0x8000000000000000ULL);
    u8 code[] = { 0x48, 0xD1, 0xE0 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0x0ULL);
    EXPECT_TRUE(m_cpu->flags.cf());
}

TEST_F(ShiftInterpreterTest, ShlSetsZF) {
    // SHL EAX, 1 where EAX = 0x80000000 → result 0 → ZF set
    // D1 E0 = SHL EAX, 1
    m_cpu->gpr.set64(0, 0x80000000);
    u8 code[] = { 0xD1, 0xE0 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0x0);
    EXPECT_TRUE(m_cpu->flags.zf());
    EXPECT_TRUE(m_cpu->flags.cf());
}

TEST_F(ShiftInterpreterTest, ShlCountZeroNoFlagChange) {
    // SHL RAX, 0 should not change flags
    // 48 C1 E0 00 = SHL RAX, 0
    m_cpu->gpr.set64(0, 0x1234);
    m_cpu->flags.set_cf(true);
    m_cpu->flags.set_zf(true);
    u8 code[] = { 0x48, 0xC1, 0xE0, 0x00 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0x1234);
    EXPECT_TRUE(m_cpu->flags.cf());  // Unchanged
    EXPECT_TRUE(m_cpu->flags.zf());  // Unchanged
}

TEST_F(ShiftInterpreterTest, LegacySlash6ShlBhCountZeroPreservesValueAndFlags) {
    // C0 F7 00 = the legacy /6 alias of SHL BH, 0.
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), 0x00008100);
    m_cpu->flags.set_cf(true);
    m_cpu->flags.set_zf(true);
    u8 code[] = { 0xC0, 0xF7, 0x00 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get8h(static_cast<u8>(RegIndex::RBX)), 0x81);
    EXPECT_TRUE(m_cpu->flags.cf());
    EXPECT_TRUE(m_cpu->flags.zf());
}

TEST_F(ShiftInterpreterTest, LegacySlash6ShlBhMatchesShlSemantics) {
    // C0 F7 01 = the legacy /6 alias of SHL BH, 1.
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), 0x00008100);
    u8 code[] = { 0xC0, 0xF7, 0x01 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get8h(static_cast<u8>(RegIndex::RBX)), 0x02);
    EXPECT_TRUE(m_cpu->flags.cf());
}

TEST_F(ShiftInterpreterTest, ShlSetsSF) {
    // SHL EAX, 1 where bit 30 is set → MSB set → SF
    // D1 E0 = SHL EAX, 1
    m_cpu->gpr.set64(0, 0x40000000);
    u8 code[] = { 0xD1, 0xE0 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0x80000000);
    EXPECT_TRUE(m_cpu->flags.sf());
    EXPECT_FALSE(m_cpu->flags.zf());
}

TEST_F(ShiftInterpreterTest, ShlOf_Count1) {
    // SHL EAX, 1: OF = MSB(result) XOR CF
    // If value = 0xC0000000 → CF=1 (bit 31 shifted out), result = 0x80000000 → MSB=1 → OF = 1^1 = 0
    m_cpu->gpr.set64(0, 0xC0000000);
    u8 code[] = { 0xD1, 0xE0 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0x80000000);
    EXPECT_TRUE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.of());
}

TEST_F(ShiftInterpreterTest, ShlOf_Count1_Overflow) {
    // SHL EAX, 1: value = 0x40000000 → CF=0, result=0x80000000 → MSB=1 → OF = 1^0 = 1
    m_cpu->gpr.set64(0, 0x40000000);
    u8 code[] = { 0xD1, 0xE0 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_TRUE(m_cpu->flags.of());
}

TEST_F(ShiftInterpreterTest, ShlAl) {
    // C0 E0 02 = SHL AL, 2
    m_cpu->gpr.set64(0, 0x0F);
    u8 code[] = { 0xC0, 0xE0, 0x02 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0) & 0xFF, 0x3C);
}

// --- SHR execution tests ---

TEST_F(ShiftInterpreterTest, ShrBasic) {
    // 48 C1 E8 04 = SHR RAX, 4
    m_cpu->gpr.set64(0, 0xFF0);
    u8 code[] = { 0x48, 0xC1, 0xE8, 0x04 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0xFF);
}

TEST_F(ShiftInterpreterTest, ShrSetsCF) {
    // SHR RAX, 1 where bit 0 is set → CF = 1
    // 48 D1 E8 = SHR RAX, 1
    m_cpu->gpr.set64(0, 0x1);
    u8 code[] = { 0x48, 0xD1, 0xE8 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0x0);
    EXPECT_TRUE(m_cpu->flags.cf());
    EXPECT_TRUE(m_cpu->flags.zf());
}

TEST_F(ShiftInterpreterTest, ShrSetsZF) {
    // SHR EAX, 1 where EAX = 1 → result 0 → ZF
    // D1 E8 = SHR EAX, 1
    m_cpu->gpr.set64(0, 0x1);
    u8 code[] = { 0xD1, 0xE8 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0x0);
    EXPECT_TRUE(m_cpu->flags.zf());
}

TEST_F(ShiftInterpreterTest, ShrOf_Count1_MsbSet) {
    // SHR EAX, 1: OF = MSB of original. Value = 0x80000000 → OF = 1
    m_cpu->gpr.set64(0, 0x80000000);
    u8 code[] = { 0xD1, 0xE8 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0x40000000);
    EXPECT_TRUE(m_cpu->flags.of());
    EXPECT_FALSE(m_cpu->flags.cf());
}

TEST_F(ShiftInterpreterTest, ShrOf_Count1_MsbClear) {
    // SHR EAX, 1: OF = MSB of original. Value = 0x40000000 → OF = 0
    m_cpu->gpr.set64(0, 0x40000000);
    u8 code[] = { 0xD1, 0xE8 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0x20000000);
    EXPECT_FALSE(m_cpu->flags.of());
}

TEST_F(ShiftInterpreterTest, ShrCountZeroNoFlagChange) {
    // 48 C1 E8 00 = SHR RAX, 0
    m_cpu->gpr.set64(0, 0xABCD);
    m_cpu->flags.set_cf(true);
    u8 code[] = { 0x48, 0xC1, 0xE8, 0x00 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0xABCD);
    EXPECT_TRUE(m_cpu->flags.cf());
}

// --- SAR execution tests ---

TEST_F(ShiftInterpreterTest, SarPreservesSign) {
    // SAR RAX, 4 with positive value
    // 48 C1 F8 04 = SAR RAX, 4
    m_cpu->gpr.set64(0, 0x100);
    u8 code[] = { 0x48, 0xC1, 0xF8, 0x04 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0x10);
}

TEST_F(ShiftInterpreterTest, SarNegativeValue) {
    // SAR RAX, 4 with negative value (sign extension)
    // 48 C1 F8 04 = SAR RAX, 4
    m_cpu->gpr.set64(0, 0xFFFFFFFFFFFFFFF0ULL);  // -16
    u8 code[] = { 0x48, 0xC1, 0xF8, 0x04 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0xFFFFFFFFFFFFFFFFULL);  // -1
    EXPECT_TRUE(m_cpu->flags.sf());
}

TEST_F(ShiftInterpreterTest, SarNegativeValue32) {
    // SAR EAX, 4 with negative 32-bit value
    // C1 F8 04 = SAR EAX, 4
    m_cpu->gpr.set64(0, 0xFFFFFFF0);  // -16 in 32-bit
    u8 code[] = { 0xC1, 0xF8, 0x04 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0xFFFFFFFF);  // -1 in 32-bit (zero-extended to 64)
}

TEST_F(ShiftInterpreterTest, SarSetsCF) {
    // SAR RAX, 1 where bit 0 is set
    // 48 D1 F8 = SAR RAX, 1
    m_cpu->gpr.set64(0, 0x3);
    u8 code[] = { 0x48, 0xD1, 0xF8 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0x1);
    EXPECT_TRUE(m_cpu->flags.cf());
}

TEST_F(ShiftInterpreterTest, SarOf_Count1_AlwaysZero) {
    // SAR EAX, 1: OF always 0 for count=1
    m_cpu->gpr.set64(0, 0x80000000);
    u8 code[] = { 0xD1, 0xF8 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_FALSE(m_cpu->flags.of());
}

TEST_F(ShiftInterpreterTest, SarCountZeroNoFlagChange) {
    // 48 C1 F8 00 = SAR RAX, 0
    m_cpu->gpr.set64(0, 0xDEAD);
    m_cpu->flags.set_cf(true);
    u8 code[] = { 0x48, 0xC1, 0xF8, 0x00 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0xDEAD);
    EXPECT_TRUE(m_cpu->flags.cf());
}

// --- CL-based shift tests ---

TEST_F(ShiftInterpreterTest, ShlByCl) {
    // D3 E0 = SHL EAX, CL
    m_cpu->gpr.set64(0, 0x1);       // EAX = 1
    m_cpu->gpr.set64(1, 0x4);       // ECX (CL = 4)
    u8 code[] = { 0xD3, 0xE0 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0x10);
}

TEST_F(ShiftInterpreterTest, ShrByCl) {
    // D3 E8 = SHR EAX, CL
    m_cpu->gpr.set64(0, 0x100);     // EAX = 256
    m_cpu->gpr.set64(1, 0x4);       // ECX (CL = 4)
    u8 code[] = { 0xD3, 0xE8 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0x10);
}

TEST_F(ShiftInterpreterTest, SarByCl) {
    // 48 D3 F8 = SAR RAX, CL
    m_cpu->gpr.set64(0, 0xFFFFFFFFFFFFFFF0ULL);  // -16
    m_cpu->gpr.set64(1, 0x2);                      // CL = 2
    u8 code[] = { 0x48, 0xD3, 0xF8 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0xFFFFFFFFFFFFFFFCULL);  // -4
}

TEST_F(ShiftInterpreterTest, ShlByClMasked) {
    // CL = 0x44 (68), masked to 68 & 31 = 4 for 32-bit
    // D3 E0 = SHL EAX, CL
    m_cpu->gpr.set64(0, 0x1);
    m_cpu->gpr.set64(1, 0x44);
    u8 code[] = { 0xD3, 0xE0 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0x10);  // 1 << 4 = 16
}

TEST_F(ShiftInterpreterTest, ShlByClMasked64) {
    // CL = 0x44 (68), masked to 68 & 63 = 4 for 64-bit
    // 48 D3 E0 = SHL RAX, CL
    m_cpu->gpr.set64(0, 0x1);
    m_cpu->gpr.set64(1, 0x44);
    u8 code[] = { 0x48, 0xD3, 0xE0 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0x10);  // 1 << 4 = 16
}

// --- 8-bit interpreter tests ---

TEST_F(ShiftInterpreterTest, ShrAl) {
    // C0 E8 02 = SHR AL, 2
    m_cpu->gpr.set64(0, 0xFC);
    u8 code[] = { 0xC0, 0xE8, 0x02 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0) & 0xFF, 0x3F);
}

TEST_F(ShiftInterpreterTest, SarAl_Negative) {
    // C0 F8 02 = SAR AL, 2
    m_cpu->gpr.set64(0, 0xF0);  // -16 in i8
    u8 code[] = { 0xC0, 0xF8, 0x02 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0) & 0xFF, 0xFC);  // -4 in i8
}

// ============================================================================
// SHRD (Double-precision shift right) Tests
// Ed25519 field arithmetic uses SHRD for multi-precision arithmetic
// ============================================================================

TEST_F(ShiftInterpreterTest, Shrd32Imm8_Basic) {
    // SHRD EAX, EDX, 4
    // Result: (EAX >> 4) | (EDX << 28)
    // 0F AC D0 04: SHRD EAX, EDX, 4
    m_cpu->gpr.set32(0, 0x12345678);  // EAX
    m_cpu->gpr.set32(2, 0xABCDEF00);  // EDX
    u8 code[] = { 0x0F, 0xAC, 0xD0, 0x04 };  // SHRD EAX, EDX, 4
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // EAX >> 4 = 0x01234567
    // EDX << 28 = 0x00000000 (low 4 bits of EDX = 0)
    // Result = 0x01234567
    EXPECT_EQ(m_cpu->gpr.get32(0), 0x01234567u);
}

TEST_F(ShiftInterpreterTest, Shrd32Imm8_CrossBits) {
    // SHRD EDI, ESI, 30 (shift by 30 bits)
    // Result: (EDI >> 30) | (ESI << 2)
    // 0F AC F7 1E: SHRD EDI, ESI, 30 (0x1E)
    m_cpu->gpr.set32(7, 0x80000000);  // EDI - high bit set
    m_cpu->gpr.set32(6, 0x12345678);  // ESI
    u8 code[] = { 0x0F, 0xAC, 0xF7, 0x1E };  // SHRD EDI, ESI, 30
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // EDI >> 30 = 0x00000002 (high 2 bits)
    // ESI << 2 = 0x48D159E0
    // Result = 0x48D159E0 | 0x00000002 = 0x48D159E2
    EXPECT_EQ(m_cpu->gpr.get32(7), 0x48D159E2u);
}

TEST_F(ShiftInterpreterTest, Shrd32Imm8_Ed25519Pattern) {
    // Ed25519 uses SHRD with shift=30 (0x1E) for field element reduction
    // e.g., shrd  esi, edi, 0x1e
    // 0F AC FE 1E: SHRD ESI, EDI, 30
    m_cpu->gpr.set32(6, 0xFFFFFFFF);  // ESI
    m_cpu->gpr.set32(7, 0x00000003);  // EDI
    u8 code[] = { 0x0F, 0xAC, 0xFE, 0x1E };  // SHRD ESI, EDI, 30
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // ESI >> 30 = 0x00000003
    // EDI << 2 = 0x0000000C
    // Result = 0x0000000F
    EXPECT_EQ(m_cpu->gpr.get32(6), 0x0000000Fu);
}

TEST_F(ShiftInterpreterTest, Shrd32Imm8_ZeroShift) {
    // SHRD with count=0 should do nothing
    m_cpu->gpr.set32(0, 0x12345678);  // EAX
    m_cpu->gpr.set32(2, 0xABCDEF00);  // EDX
    u8 code[] = { 0x0F, 0xAC, 0xD0, 0x00 };  // SHRD EAX, EDX, 0
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(0), 0x12345678u);  // Unchanged
}

TEST_F(ShiftInterpreterTest, Shrd32Imm8_ShiftBy1) {
    // SHRD with count=1 checks OF flag
    m_cpu->gpr.set32(0, 0x12345678);  // EAX
    m_cpu->gpr.set32(2, 0x00000001);  // EDX - low bit set
    u8 code[] = { 0x0F, 0xAC, 0xD0, 0x01 };  // SHRD EAX, EDX, 1
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // EAX >> 1 = 0x091A2B3C
    // EDX << 31 = 0x80000000
    // Result = 0x891A2B3C
    EXPECT_EQ(m_cpu->gpr.get32(0), 0x891A2B3Cu);
    // CF = last bit shifted out = bit 0 of original EAX = 0
    EXPECT_FALSE(m_cpu->flags.cf());
}

TEST_F(ShiftInterpreterTest, Shrd32Imm8_CfSet) {
    // Test CF is set when bit is shifted out
    m_cpu->gpr.set32(0, 0x00000001);  // EAX - low bit set
    m_cpu->gpr.set32(2, 0x00000000);  // EDX
    u8 code[] = { 0x0F, 0xAC, 0xD0, 0x01 };  // SHRD EAX, EDX, 1
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(0), 0x00000000u);
    EXPECT_TRUE(m_cpu->flags.cf());  // Bit 0 was shifted out
}

TEST_F(ShiftInterpreterTest, Shrd32Imm8_FullMultiPrecision) {
    // Test a realistic multi-precision modular reduction scenario
    // Simulates: result = (high:low) >> 30
    // This is exactly what Ed25519 field element representation does
    m_cpu->gpr.set32(0, 0xC0000000);  // EAX (low) - upper 2 bits set
    m_cpu->gpr.set32(2, 0x0000000F);  // EDX (high) - 4 bits
    u8 code[] = { 0x0F, 0xAC, 0xD0, 0x1E };  // SHRD EAX, EDX, 30
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // EAX >> 30 = 0x00000003
    // EDX << 2 = 0x0000003C
    // Result = 0x0000003F
    EXPECT_EQ(m_cpu->gpr.get32(0), 0x0000003Fu);
}
