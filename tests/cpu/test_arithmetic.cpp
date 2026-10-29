// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Arithmetic instruction tests (ADD/SUB) - CPU-011

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
// ADD Decoder Tests
// ============================================================================

TEST(ArithmeticDecoder, AddRm8R8) {
    // 00 D8 - ADD AL, BL
    auto inst = decode({0x00, 0xD8});
    EXPECT_EQ(inst.opcode, Opcode::ADD_RM8_R8);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[0].reg, 0);  // AL
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[1].reg, 3);  // BL
}

TEST(ArithmeticDecoder, AddR8Rm8) {
    // 02 D8 - ADD BL, AL
    auto inst = decode({0x02, 0xD8});
    EXPECT_EQ(inst.opcode, Opcode::ADD_R8_RM8);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[0].reg, 3);  // BL
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[1].reg, 0);  // AL
}

TEST(ArithmeticDecoder, AddRm64R64) {
    // 48 01 D8 - ADD RAX, RBX
    auto inst = decode({0x48, 0x01, 0xD8});
    EXPECT_EQ(inst.opcode, Opcode::ADD_RM64_R64);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 0);  // RAX
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[1].reg, 3);  // RBX
}

TEST(ArithmeticDecoder, AddR64Rm64) {
    // 48 03 D8 - ADD RBX, RAX
    auto inst = decode({0x48, 0x03, 0xD8});
    EXPECT_EQ(inst.opcode, Opcode::ADD_R64_RM64);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 3);  // RBX
}

TEST(ArithmeticDecoder, AddAlImm8) {
    // 04 42 - ADD AL, 0x42
    auto inst = decode({0x04, 0x42});
    EXPECT_EQ(inst.opcode, Opcode::ADD_AL_IMM8);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[1].imm, 0x42);
}

TEST(ArithmeticDecoder, AddEaxImm32) {
    // 05 78 56 34 12 - ADD EAX, 0x12345678
    auto inst = decode({0x05, 0x78, 0x56, 0x34, 0x12});
    EXPECT_EQ(inst.opcode, Opcode::ADD_EAX_IMM32);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].imm, 0x12345678);
}

TEST(ArithmeticDecoder, AddRaxImm32) {
    // 48 05 78 56 34 12 - ADD RAX, 0x12345678
    auto inst = decode({0x48, 0x05, 0x78, 0x56, 0x34, 0x12});
    EXPECT_EQ(inst.opcode, Opcode::ADD_RAX_IMM32);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
}

TEST(ArithmeticDecoder, AddRm64Imm8_Group1) {
    // 48 83 C0 10 - ADD RAX, 0x10 (Group 1, /0)
    auto inst = decode({0x48, 0x83, 0xC0, 0x10});
    EXPECT_EQ(inst.opcode, Opcode::ADD_RM64_IMM8);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 0);  // RAX
    EXPECT_EQ(inst.operands[1].imm, 0x10);
}

TEST(ArithmeticDecoder, AddRm32Imm32_Group1) {
    // 81 C0 78 56 34 12 - ADD EAX, 0x12345678 (Group 1, /0)
    auto inst = decode({0x81, 0xC0, 0x78, 0x56, 0x34, 0x12});
    EXPECT_EQ(inst.opcode, Opcode::ADD_RM32_IMM32);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].imm, 0x12345678);
}

TEST(ArithmeticDecoder, AddRm8Imm8_Group1) {
    // 80 C0 42 - ADD AL, 0x42 (Group 1, /0)
    auto inst = decode({0x80, 0xC0, 0x42});
    EXPECT_EQ(inst.opcode, Opcode::ADD_RM8_IMM8);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[1].imm, 0x42);
}

// ============================================================================
// SUB Decoder Tests
// ============================================================================

TEST(ArithmeticDecoder, SubRm8R8) {
    // 28 D8 - SUB AL, BL
    auto inst = decode({0x28, 0xD8});
    EXPECT_EQ(inst.opcode, Opcode::SUB_RM8_R8);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[0].reg, 0);  // AL
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[1].reg, 3);  // BL
}

TEST(ArithmeticDecoder, SubR8Rm8) {
    // 2A D8 - SUB BL, AL
    auto inst = decode({0x2A, 0xD8});
    EXPECT_EQ(inst.opcode, Opcode::SUB_R8_RM8);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[0].reg, 3);  // BL
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[1].reg, 0);  // AL
}

TEST(ArithmeticDecoder, SubRm64R64) {
    // 48 29 D8 - SUB RAX, RBX
    auto inst = decode({0x48, 0x29, 0xD8});
    EXPECT_EQ(inst.opcode, Opcode::SUB_RM64_R64);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 0);  // RAX
}

TEST(ArithmeticDecoder, SubR64Rm64) {
    // 48 2B D8 - SUB RBX, RAX
    auto inst = decode({0x48, 0x2B, 0xD8});
    EXPECT_EQ(inst.opcode, Opcode::SUB_R64_RM64);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 3);  // RBX
}

TEST(ArithmeticDecoder, SubAlImm8) {
    // 2C 42 - SUB AL, 0x42
    auto inst = decode({0x2C, 0x42});
    EXPECT_EQ(inst.opcode, Opcode::SUB_AL_IMM8);
    EXPECT_EQ(inst.operands[1].imm, 0x42);
}

TEST(ArithmeticDecoder, SubEaxImm32) {
    // 2D 78 56 34 12 - SUB EAX, 0x12345678
    auto inst = decode({0x2D, 0x78, 0x56, 0x34, 0x12});
    EXPECT_EQ(inst.opcode, Opcode::SUB_EAX_IMM32);
    EXPECT_EQ(inst.operands[1].imm, 0x12345678);
}

TEST(ArithmeticDecoder, SubRm64Imm8_Group1) {
    // 48 83 E8 10 - SUB RAX, 0x10 (Group 1, /5)
    auto inst = decode({0x48, 0x83, 0xE8, 0x10});
    EXPECT_EQ(inst.opcode, Opcode::SUB_RM64_IMM8);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 0);  // RAX
    EXPECT_EQ(inst.operands[1].imm, 0x10);
}

TEST(ArithmeticDecoder, SubRm32Imm32_Group1) {
    // 81 E8 78 56 34 12 - SUB EAX, 0x12345678 (Group 1, /5)
    auto inst = decode({0x81, 0xE8, 0x78, 0x56, 0x34, 0x12});
    EXPECT_EQ(inst.opcode, Opcode::SUB_RM32_IMM32);
    EXPECT_EQ(inst.operands[1].imm, 0x12345678);
}

TEST(ArithmeticDecoder, SubRm8Imm8_Group1) {
    // 80 E8 42 - SUB AL, 0x42 (Group 1, /5)
    auto inst = decode({0x80, 0xE8, 0x42});
    EXPECT_EQ(inst.opcode, Opcode::SUB_RM8_IMM8);
    EXPECT_EQ(inst.operands[1].imm, 0x42);
}

// ============================================================================
// Group 1 Other Operations Tests
// ============================================================================

TEST(ArithmeticDecoder, CmpRm64Imm8_Group1) {
    // 48 83 F8 10 - CMP RAX, 0x10 (Group 1, /7)
    auto inst = decode({0x48, 0x83, 0xF8, 0x10});
    EXPECT_EQ(inst.opcode, Opcode::CMP_RM64_IMM8);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 0);  // RAX
}

TEST(ArithmeticDecoder, AndRm64Imm8_Group1) {
    // 48 83 E0 0F - AND RAX, 0x0F (Group 1, /4)
    auto inst = decode({0x48, 0x83, 0xE0, 0x0F});
    EXPECT_EQ(inst.opcode, Opcode::AND_RM64_IMM8);
}

TEST(ArithmeticDecoder, OrRm64Imm8_Group1) {
    // 48 83 C8 0F - OR RAX, 0x0F (Group 1, /1)
    auto inst = decode({0x48, 0x83, 0xC8, 0x0F});
    EXPECT_EQ(inst.opcode, Opcode::OR_RM64_IMM8);
}

TEST(ArithmeticDecoder, XorRm64Imm8_Group1) {
    // 48 83 F0 FF - XOR RAX, -1 (Group 1, /6)
    auto inst = decode({0x48, 0x83, 0xF0, 0xFF});
    EXPECT_EQ(inst.opcode, Opcode::XOR_RM64_IMM8);
}

// ============================================================================
// Interpreter Tests - ADD
// ============================================================================

class ArithmeticInterpreterTest : public ::testing::Test {
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

TEST_F(ArithmeticInterpreterTest, AddR64R64Basic) {
    // ADD RAX, RBX: 48 01 D8
    m_cpu->gpr.set64(0, 100);  // RAX
    m_cpu->gpr.set64(3, 50);   // RBX
    
    u8 code[] = { 0x48, 0x01, 0xD8 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 150);
    EXPECT_FALSE(m_cpu->flags.zf());
    EXPECT_FALSE(m_cpu->flags.sf());
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.of());
}

TEST_F(ArithmeticInterpreterTest, AddSetsZeroFlag) {
    // ADD RAX, RBX where RAX=0, RBX=0
    m_cpu->gpr.set64(0, 0);
    m_cpu->gpr.set64(3, 0);
    
    u8 code[] = { 0x48, 0x01, 0xD8 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0);
    EXPECT_TRUE(m_cpu->flags.zf());
}

TEST_F(ArithmeticInterpreterTest, AddSetsSignFlag) {
    // ADD RAX, RBX where result is negative
    m_cpu->gpr.set64(0, static_cast<u64>(-100));
    m_cpu->gpr.set64(3, 50);
    
    u8 code[] = { 0x48, 0x01, 0xD8 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(static_cast<i64>(m_cpu->gpr.get64(0)), -50);
    EXPECT_TRUE(m_cpu->flags.sf());
}

TEST_F(ArithmeticInterpreterTest, AddSetsCarryFlag) {
    // ADD RAX, RBX with unsigned overflow
    m_cpu->gpr.set64(0, 0xFFFFFFFFFFFFFFFFULL);
    m_cpu->gpr.set64(3, 1);
    
    u8 code[] = { 0x48, 0x01, 0xD8 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0);
    EXPECT_TRUE(m_cpu->flags.cf());
    EXPECT_TRUE(m_cpu->flags.zf());
}

TEST_F(ArithmeticInterpreterTest, AddSetsOverflowFlag) {
    // ADD RAX, RBX with signed overflow (two large positive numbers)
    m_cpu->gpr.set64(0, 0x7FFFFFFFFFFFFFFFULL);  // MAX_INT64
    m_cpu->gpr.set64(3, 1);
    
    u8 code[] = { 0x48, 0x01, 0xD8 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_TRUE(m_cpu->flags.of());
    EXPECT_TRUE(m_cpu->flags.sf());  // Result wraps to negative
}

TEST_F(ArithmeticInterpreterTest, Add8Bit) {
    // ADD AL, BL: 00 D8
    m_cpu->gpr.set8l(0, 100);  // AL
    m_cpu->gpr.set8l(3, 50);   // BL
    
    u8 code[] = { 0x00, 0xD8 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get8l(0), 150);
}

TEST_F(ArithmeticInterpreterTest, Add8BitOverflow) {
    // ADD AL, BL with 8-bit overflow
    m_cpu->gpr.set8l(0, 200);
    m_cpu->gpr.set8l(3, 100);
    
    u8 code[] = { 0x00, 0xD8 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get8l(0), 44);  // 300 mod 256
    EXPECT_TRUE(m_cpu->flags.cf());
}

TEST_F(ArithmeticInterpreterTest, Add32BitZeroExtends) {
    // ADD EAX, EBX should zero-extend to 64 bits: 01 D8
    m_cpu->gpr.set64(0, 0xFFFFFFFF00000064ULL);  // Set upper bits
    m_cpu->gpr.set32(3, 50);
    
    u8 code[] = { 0x01, 0xD8 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 150);  // Upper bits cleared
}

TEST_F(ArithmeticInterpreterTest, AddAlImm8) {
    // ADD AL, 0x42: 04 42
    m_cpu->gpr.set8l(0, 10);
    
    u8 code[] = { 0x04, 0x42 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get8l(0), 10 + 0x42);
}

TEST_F(ArithmeticInterpreterTest, AddRaxImm32SignExtended) {
    // ADD RAX, -10 via Group 1: 48 83 C0 F6
    m_cpu->gpr.set64(0, 100);
    
    u8 code[] = { 0x48, 0x83, 0xC0, 0xF6 };  // -10 as signed imm8
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 90);
}

// ============================================================================
// Interpreter Tests - SUB
// ============================================================================

TEST_F(ArithmeticInterpreterTest, SubR64R64Basic) {
    // SUB RAX, RBX: 48 29 D8
    m_cpu->gpr.set64(0, 100);
    m_cpu->gpr.set64(3, 50);
    
    u8 code[] = { 0x48, 0x29, 0xD8 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 50);
    EXPECT_FALSE(m_cpu->flags.zf());
    EXPECT_FALSE(m_cpu->flags.sf());
    EXPECT_FALSE(m_cpu->flags.cf());
}

TEST_F(ArithmeticInterpreterTest, SubSetsZeroFlag) {
    // SUB RAX, RBX where RAX == RBX
    m_cpu->gpr.set64(0, 100);
    m_cpu->gpr.set64(3, 100);
    
    u8 code[] = { 0x48, 0x29, 0xD8 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0);
    EXPECT_TRUE(m_cpu->flags.zf());
}

TEST_F(ArithmeticInterpreterTest, SubSetsBorrowFlag) {
    // SUB RAX, RBX where RAX < RBX (unsigned)
    m_cpu->gpr.set64(0, 50);
    m_cpu->gpr.set64(3, 100);
    
    u8 code[] = { 0x48, 0x29, 0xD8 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_TRUE(m_cpu->flags.cf());  // Borrow
    EXPECT_TRUE(m_cpu->flags.sf());  // Negative result
}

TEST_F(ArithmeticInterpreterTest, SubSetsOverflowFlag) {
    // SUB RAX, RBX with signed overflow
    // Subtracting large negative from large positive should overflow
    m_cpu->gpr.set64(0, 0x7FFFFFFFFFFFFFFFULL);  // MAX_INT64
    m_cpu->gpr.set64(3, static_cast<u64>(-1));   // -1
    
    u8 code[] = { 0x48, 0x29, 0xD8 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_TRUE(m_cpu->flags.of());
}

TEST_F(ArithmeticInterpreterTest, Sub8Bit) {
    // SUB AL, BL: 28 D8
    m_cpu->gpr.set8l(0, 100);
    m_cpu->gpr.set8l(3, 30);
    
    u8 code[] = { 0x28, 0xD8 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get8l(0), 70);
}

TEST_F(ArithmeticInterpreterTest, SubAlImm8) {
    // SUB AL, 0x10: 2C 10
    m_cpu->gpr.set8l(0, 0x42);
    
    u8 code[] = { 0x2C, 0x10 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get8l(0), 0x32);
}

TEST_F(ArithmeticInterpreterTest, SubRaxImm8SignExtended) {
    // SUB RAX, -10 via Group 1: 48 83 E8 F6
    m_cpu->gpr.set64(0, 100);
    
    u8 code[] = { 0x48, 0x83, 0xE8, 0xF6 };  // -10 as signed imm8
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // 100 - (-10) = 110
    EXPECT_EQ(m_cpu->gpr.get64(0), 110);
}

// ============================================================================
// Parity Flag Tests
// ============================================================================

TEST_F(ArithmeticInterpreterTest, ParityFlagEvenBits) {
    // Result with even number of 1 bits in low byte
    m_cpu->gpr.set64(0, 0);
    m_cpu->gpr.set64(3, 0xFF);  // 8 bits = even
    
    u8 code[] = { 0x48, 0x01, 0xD8 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_TRUE(m_cpu->flags.pf());
}

TEST_F(ArithmeticInterpreterTest, ParityFlagOddBits) {
    // Result with odd number of 1 bits in low byte
    m_cpu->gpr.set64(0, 0);
    m_cpu->gpr.set64(3, 0x01);  // 1 bit = odd
    
    u8 code[] = { 0x48, 0x01, 0xD8 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_FALSE(m_cpu->flags.pf());
}

// ============================================================================
// Memory Operand Tests
// ============================================================================

TEST_F(ArithmeticInterpreterTest, AddMemR64) {
    // ADD [RBX], RAX: 48 01 03
    u64 mem_addr = 0x2000;
    m_cpu->gpr.set64(3, mem_addr);  // RBX = address
    m_cpu->gpr.set64(0, 50);        // RAX = value to add
    
    // Write initial value to memory
    u64 initial = 100;
    m_memory->write_bytes(mem_addr, reinterpret_cast<u8*>(&initial), 8);
    
    u8 code[] = { 0x48, 0x01, 0x03 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    
    // Read result from memory
    u64 result = 0;
    m_memory->read_bytes(mem_addr, reinterpret_cast<u8*>(&result), 8);
    EXPECT_EQ(result, 150);
}

TEST_F(ArithmeticInterpreterTest, SubR64Mem) {
    // SUB RAX, [RBX]: 48 2B 03
    u64 mem_addr = 0x2000;
    m_cpu->gpr.set64(0, 100);       // RAX
    m_cpu->gpr.set64(3, mem_addr);  // RBX = address
    
    // Write value to memory
    u64 mem_val = 30;
    m_memory->write_bytes(mem_addr, reinterpret_cast<u8*>(&mem_val), 8);
    
    u8 code[] = { 0x48, 0x2B, 0x03 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 70);
}

// ============================================================================
// Auxiliary Carry Flag Tests
// ============================================================================

TEST_F(ArithmeticInterpreterTest, AddSetsAuxCarry) {
    // ADD with carry from bit 3 to bit 4
    m_cpu->gpr.set8l(0, 0x0F);
    m_cpu->gpr.set8l(3, 0x01);
    
    u8 code[] = { 0x00, 0xD8 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get8l(0), 0x10);
    EXPECT_TRUE(m_cpu->flags.af());
}

TEST_F(ArithmeticInterpreterTest, SubSetsAuxCarry) {
    // SUB with borrow from bit 4 to bit 3
    m_cpu->gpr.set8l(0, 0x10);
    m_cpu->gpr.set8l(3, 0x01);
    
    u8 code[] = { 0x28, 0xD8 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get8l(0), 0x0F);
    EXPECT_TRUE(m_cpu->flags.af());
}
