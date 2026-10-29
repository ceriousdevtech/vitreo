// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Multiply/Divide instruction tests - CPU-019

#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include <gtest/gtest.h>

using namespace vitreo;
using namespace vitreo::cpu;

// ============================================================================
// Decoder helper
// ============================================================================

static DecodedInstruction decode(const std::initializer_list<u8>& bytes, DecodeMode mode = DecodeMode::Mode64) {
    std::vector<u8> data(bytes);
    MemoryByteReader reader(data.data(), data.size());
    DecodedInstruction inst;
    DecodeError err = decode_instruction(reader, mode, inst);
    EXPECT_EQ(err, DecodeError::Success) << "Decode failed";
    return inst;
}

// ============================================================================
// Decode Tests - Group 3 (MUL/IMUL/DIV/IDIV one-operand)
// ============================================================================

TEST(MulDivDecoder, MulRm8) {
    // F6 E1 = MUL CL  (reg /4, CL=1)
    auto inst = decode({0xF6, 0xE1});
    EXPECT_EQ(inst.opcode, Opcode::MUL_RM8);
    EXPECT_EQ(inst.operand_count, 1);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[0].reg, 1);  // CL
}

TEST(MulDivDecoder, MulRm32) {
    // F7 E1 = MUL ECX  (reg /4, ECX=1)
    auto inst = decode({0xF7, 0xE1});
    EXPECT_EQ(inst.opcode, Opcode::MUL_RM32);
    EXPECT_EQ(inst.operand_count, 1);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 1);  // ECX
}

TEST(MulDivDecoder, MulRm64) {
    // 48 F7 E1 = MUL RCX
    auto inst = decode({0x48, 0xF7, 0xE1});
    EXPECT_EQ(inst.opcode, Opcode::MUL_RM64);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 1);  // RCX
}

TEST(MulDivDecoder, ImulRm32_OneOp) {
    // F7 E9 = IMUL ECX  (reg /5)
    auto inst = decode({0xF7, 0xE9});
    EXPECT_EQ(inst.opcode, Opcode::IMUL_RM32);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 1);
}

TEST(MulDivDecoder, DivRm8) {
    // F6 F1 = DIV CL  (reg /6)
    auto inst = decode({0xF6, 0xF1});
    EXPECT_EQ(inst.opcode, Opcode::DIV_RM8);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[0].reg, 1);
}

TEST(MulDivDecoder, DivRm32) {
    // F7 F1 = DIV ECX  (reg /6)
    auto inst = decode({0xF7, 0xF1});
    EXPECT_EQ(inst.opcode, Opcode::DIV_RM32);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 1);
}

TEST(MulDivDecoder, IdivRm32) {
    // F7 F9 = IDIV ECX  (reg /7)
    auto inst = decode({0xF7, 0xF9});
    EXPECT_EQ(inst.opcode, Opcode::IDIV_RM32);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 1);
}

// ============================================================================
// Decode Tests - IMUL two-operand (0F AF /r)
// ============================================================================

TEST(MulDivDecoder, ImulTwoOp_R32_RM32) {
    // 0F AF C1 = IMUL EAX, ECX
    auto inst = decode({0x0F, 0xAF, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::IMUL_R32_RM32);
    EXPECT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 0);  // EAX
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].reg, 1);  // ECX
}

TEST(MulDivDecoder, ImulTwoOp_R64_RM64) {
    // 48 0F AF C1 = IMUL RAX, RCX
    auto inst = decode({0x48, 0x0F, 0xAF, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::IMUL_R64_RM64);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 0);  // RAX
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[1].reg, 1);  // RCX
}

// ============================================================================
// Decode Tests - IMUL three-operand (6B /r ib, 69 /r id)
// ============================================================================

TEST(MulDivDecoder, ImulThreeOp_R32_RM32_Imm8) {
    // 6B C1 05 = IMUL EAX, ECX, 5
    auto inst = decode({0x6B, 0xC1, 0x05});
    EXPECT_EQ(inst.opcode, Opcode::IMUL_R32_RM32_IMM8);
    EXPECT_EQ(inst.operand_count, 3);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 0);  // EAX
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].reg, 1);  // ECX
    EXPECT_EQ(inst.operands[2].type, OperandType::Imm8);
    EXPECT_EQ(inst.operands[2].imm, 5);
}

TEST(MulDivDecoder, ImulThreeOp_R32_RM32_Imm32) {
    // 69 C1 E8 03 00 00 = IMUL EAX, ECX, 1000
    auto inst = decode({0x69, 0xC1, 0xE8, 0x03, 0x00, 0x00});
    EXPECT_EQ(inst.opcode, Opcode::IMUL_R32_RM32_IMM32);
    EXPECT_EQ(inst.operand_count, 3);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 0);
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].reg, 1);
    EXPECT_EQ(inst.operands[2].imm, 1000);
}

TEST(MulDivDecoder, ImulThreeOp_R64_RM64_Imm8) {
    // 48 6B C1 05 = IMUL RAX, RCX, 5
    auto inst = decode({0x48, 0x6B, 0xC1, 0x05});
    EXPECT_EQ(inst.opcode, Opcode::IMUL_R64_RM64_IMM8);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 0);  // RAX
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[1].reg, 1);  // RCX
    EXPECT_EQ(inst.operands[2].imm, 5);
}

TEST(MulDivDecoder, ImulThreeOp_NegImm8) {
    // 6B C1 FB = IMUL EAX, ECX, -5
    auto inst = decode({0x6B, 0xC1, 0xFB});
    EXPECT_EQ(inst.opcode, Opcode::IMUL_R32_RM32_IMM8);
    EXPECT_EQ(inst.operands[2].imm, -5);
}

// ============================================================================
// Decode Tests - Sign Extension (0x98, 0x99)
// ============================================================================

TEST(MulDivDecoder, Cbw) {
    // 66 98 = CBW (16-bit operand size)
    auto inst = decode({0x66, 0x98});
    EXPECT_EQ(inst.opcode, Opcode::CBW);
    EXPECT_EQ(inst.operand_count, 0);
}

TEST(MulDivDecoder, Cwde) {
    // 98 = CWDE (default 32-bit operand size)
    auto inst = decode({0x98});
    EXPECT_EQ(inst.opcode, Opcode::CWDE);
    EXPECT_EQ(inst.operand_count, 0);
}

TEST(MulDivDecoder, Cdqe) {
    // 48 98 = CDQE (REX.W gives 64-bit)
    auto inst = decode({0x48, 0x98});
    EXPECT_EQ(inst.opcode, Opcode::CDQE);
    EXPECT_EQ(inst.operand_count, 0);
}

TEST(MulDivDecoder, Cwd) {
    // 66 99 = CWD
    auto inst = decode({0x66, 0x99});
    EXPECT_EQ(inst.opcode, Opcode::CWD);
}

TEST(MulDivDecoder, Cdq) {
    // 99 = CDQ
    auto inst = decode({0x99});
    EXPECT_EQ(inst.opcode, Opcode::CDQ);
}

TEST(MulDivDecoder, Cqo) {
    // 48 99 = CQO
    auto inst = decode({0x48, 0x99});
    EXPECT_EQ(inst.opcode, Opcode::CQO);
}

// ============================================================================
// Interpreter Test Fixture
// ============================================================================

class MulDivTest : public ::testing::Test {
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
// MUL Execution Tests
// ============================================================================

TEST_F(MulDivTest, MulRm8_Basic) {
    // F6 E1 = MUL CL  → AX = AL * CL
    m_cpu->gpr.set8l(static_cast<u8>(RegIndex::RAX), 5);   // AL = 5
    m_cpu->gpr.set8l(static_cast<u8>(RegIndex::RCX), 3);   // CL = 3
    u8 code[] = { 0xF6, 0xE1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get16(static_cast<u8>(RegIndex::RAX)), 15u);
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.of());
}

TEST_F(MulDivTest, MulRm8_UpperNonZero) {
    // MUL CL with AL=200, CL=2 → AX=400, AH != 0 → CF=OF=1
    m_cpu->gpr.set8l(static_cast<u8>(RegIndex::RAX), 200);
    m_cpu->gpr.set8l(static_cast<u8>(RegIndex::RCX), 2);
    u8 code[] = { 0xF6, 0xE1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get16(static_cast<u8>(RegIndex::RAX)), 400u);
    EXPECT_TRUE(m_cpu->flags.cf());
    EXPECT_TRUE(m_cpu->flags.of());
}

TEST_F(MulDivTest, MulRm32_Overflow) {
    // F7 E1 = MUL ECX  → EDX:EAX = EAX * ECX
    // 0x80000000 * 2 = 0x100000000 → EDX=1, EAX=0
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RAX), 0x80000000ULL);
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RCX), 2);
    u8 code[] = { 0xF7, 0xE1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // set32 zero-extends, so check full 64-bit
    EXPECT_EQ(m_cpu->gpr.get64(static_cast<u8>(RegIndex::RAX)), 0x00000000ULL);
    EXPECT_EQ(m_cpu->gpr.get64(static_cast<u8>(RegIndex::RDX)), 0x00000001ULL);
    EXPECT_TRUE(m_cpu->flags.cf());
    EXPECT_TRUE(m_cpu->flags.of());
}

TEST_F(MulDivTest, MulRm64) {
    // 48 F7 E1 = MUL RCX  → RDX:RAX = RAX * RCX
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RAX), 0x100000000ULL);
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RCX), 0x100000000ULL);
    u8 code[] = { 0x48, 0xF7, 0xE1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(static_cast<u8>(RegIndex::RAX)), 0x0000000000000000ULL);
    EXPECT_EQ(m_cpu->gpr.get64(static_cast<u8>(RegIndex::RDX)), 0x0000000000000001ULL);
    EXPECT_TRUE(m_cpu->flags.cf());
    EXPECT_TRUE(m_cpu->flags.of());
}

TEST_F(MulDivTest, MulRm32_NoOverflow) {
    // F7 E1 = MUL ECX  → 10 * 20 = 200, EDX=0
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RAX), 10);
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RCX), 20);
    u8 code[] = { 0xF7, 0xE1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(static_cast<u8>(RegIndex::RAX)), 200u);
    EXPECT_EQ(m_cpu->gpr.get32(static_cast<u8>(RegIndex::RDX)), 0u);
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.of());
}

// ============================================================================
// IMUL Execution Tests - One-operand
// ============================================================================

TEST_F(MulDivTest, ImulRm32_OneOp_Positive) {
    // F7 E9 = IMUL ECX (one-operand)
    // EAX=10, ECX=20 → EDX:EAX = 200
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RAX), 10);
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RCX), 20);
    u8 code[] = { 0xF7, 0xE9 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(static_cast<u8>(RegIndex::RAX)), 200u);
    EXPECT_EQ(m_cpu->gpr.get32(static_cast<u8>(RegIndex::RDX)), 0u);
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.of());
}

TEST_F(MulDivTest, ImulRm32_OneOp_Negative) {
    // IMUL ECX: EAX=-3 (0xFFFFFFFD), ECX=7 → result=-21
    // -21 fits in i32 → CF=OF=0
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RAX), static_cast<u32>(-3));
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RCX), 7);
    u8 code[] = { 0xF7, 0xE9 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(static_cast<i32>(m_cpu->gpr.get32(static_cast<u8>(RegIndex::RAX))), -21);
    EXPECT_EQ(m_cpu->gpr.get32(static_cast<u8>(RegIndex::RDX)), 0xFFFFFFFFu);  // sign extension
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.of());
}

// ============================================================================
// IMUL Execution Tests - Two-operand (0F AF /r)
// ============================================================================

TEST_F(MulDivTest, ImulTwoOp_R32_RM32) {
    // 0F AF C1 = IMUL EAX, ECX
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RAX), 7);
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RCX), 6);
    u8 code[] = { 0x0F, 0xAF, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(static_cast<u8>(RegIndex::RAX)), 42u);
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.of());
}

TEST_F(MulDivTest, ImulTwoOp_Overflow) {
    // 0F AF C1 = IMUL EAX, ECX with overflow
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RAX), 0x7FFFFFFF);  // INT32_MAX
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RCX), 2);
    u8 code[] = { 0x0F, 0xAF, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // Result truncated to 32 bits: (0x7FFFFFFF * 2) = 0xFFFFFFFE
    EXPECT_EQ(m_cpu->gpr.get32(static_cast<u8>(RegIndex::RAX)), 0xFFFFFFFEu);
    EXPECT_TRUE(m_cpu->flags.cf());
    EXPECT_TRUE(m_cpu->flags.of());
}

// ============================================================================
// IMUL Execution Tests - Three-operand
// ============================================================================

TEST_F(MulDivTest, ImulThreeOp_R32_RM32_Imm8) {
    // 6B C1 05 = IMUL EAX, ECX, 5
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RCX), 10);
    u8 code[] = { 0x6B, 0xC1, 0x05 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(static_cast<u8>(RegIndex::RAX)), 50u);
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.of());
}

TEST_F(MulDivTest, ImulThreeOp_NegImm) {
    // 6B C1 FE = IMUL EAX, ECX, -2
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RCX), 25);
    u8 code[] = { 0x6B, 0xC1, 0xFE };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(static_cast<i32>(m_cpu->gpr.get32(static_cast<u8>(RegIndex::RAX))), -50);
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.of());
}

TEST_F(MulDivTest, ImulThreeOp_R64_Imm8) {
    // 48 6B C1 0A = IMUL RAX, RCX, 10
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RCX), 100);
    u8 code[] = { 0x48, 0x6B, 0xC1, 0x0A };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(static_cast<u8>(RegIndex::RAX)), 1000u);
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.of());
}

TEST_F(MulDivTest, ImulThreeOp_R32_RM32_Imm32) {
    // 69 C1 E8 03 00 00 = IMUL EAX, ECX, 1000
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RCX), 7);
    u8 code[] = { 0x69, 0xC1, 0xE8, 0x03, 0x00, 0x00 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(static_cast<u8>(RegIndex::RAX)), 7000u);
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.of());
}

// ============================================================================
// DIV Execution Tests
// ============================================================================

TEST_F(MulDivTest, DivRm8_Basic) {
    // F6 F1 = DIV CL  → AL = AX / CL, AH = AX % CL
    // AX=100, CL=7 → AL=14, AH=2
    m_cpu->gpr.set16(static_cast<u8>(RegIndex::RAX), 100);
    m_cpu->gpr.set8l(static_cast<u8>(RegIndex::RCX), 7);
    u8 code[] = { 0xF6, 0xF1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get8l(static_cast<u8>(RegIndex::RAX)), 14u);   // AL = quotient
    EXPECT_EQ(m_cpu->gpr.get8h(0), 2u);                                 // AH = remainder
}

TEST_F(MulDivTest, DivRm32_Basic) {
    // F7 F1 = DIV ECX  → EAX = EDX:EAX / ECX, EDX = remainder
    // EDX=0, EAX=100, ECX=7 → EAX=14, EDX=2
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RAX), 100);
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RDX), 0);
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RCX), 7);
    u8 code[] = { 0xF7, 0xF1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(static_cast<u8>(RegIndex::RAX)), 14u);
    EXPECT_EQ(m_cpu->gpr.get32(static_cast<u8>(RegIndex::RDX)), 2u);
}

TEST_F(MulDivTest, DivRm32_LargeDividend) {
    // DIV ECX: EDX:EAX = 0x00000001:00000000 / 2 = 0x80000000
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RAX), 0);
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RDX), 1);
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RCX), 2);
    u8 code[] = { 0xF7, 0xF1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(static_cast<u8>(RegIndex::RAX)), 0x80000000u);
    EXPECT_EQ(m_cpu->gpr.get32(static_cast<u8>(RegIndex::RDX)), 0u);
}

TEST_F(MulDivTest, DivByZero_Exception) {
    // F7 F1 = DIV ECX with ECX=0 → #DE
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RAX), 100);
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RDX), 0);
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RCX), 0);
    u8 code[] = { 0xF7, 0xF1 };
    write_code(code, sizeof(code));

    Interpreter interp(*m_cpu, *m_memory);
    EXPECT_EQ(interp.step(), ExecResult::Exception);
}

TEST_F(MulDivTest, DivRm64_Basic) {
    // 48 F7 F1 = DIV RCX  → RAX = RDX:RAX / RCX
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RAX), 1000);
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RDX), 0);
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RCX), 7);
    u8 code[] = { 0x48, 0xF7, 0xF1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(static_cast<u8>(RegIndex::RAX)), 142u);  // 1000/7 = 142
    EXPECT_EQ(m_cpu->gpr.get64(static_cast<u8>(RegIndex::RDX)), 6u);    // 1000%7 = 6
}

// ============================================================================
// IDIV Execution Tests
// ============================================================================

TEST_F(MulDivTest, IdivRm32_Positive) {
    // F7 F9 = IDIV ECX
    // EDX:EAX = 0:100, ECX=7 → EAX=14, EDX=2
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RAX), 100);
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RDX), 0);
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RCX), 7);
    u8 code[] = { 0xF7, 0xF9 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(static_cast<i32>(m_cpu->gpr.get32(static_cast<u8>(RegIndex::RAX))), 14);
    EXPECT_EQ(static_cast<i32>(m_cpu->gpr.get32(static_cast<u8>(RegIndex::RDX))), 2);
}

TEST_F(MulDivTest, IdivRm32_Negative) {
    // IDIV ECX with negative dividend
    // EDX:EAX = -100 (EDX=0xFFFFFFFF, EAX=0xFFFFFF9C), ECX=7
    // -100 / 7 = -14, -100 % 7 = -2
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RAX), static_cast<u32>(-100));
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RDX), 0xFFFFFFFF);
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RCX), 7);
    u8 code[] = { 0xF7, 0xF9 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(static_cast<i32>(m_cpu->gpr.get32(static_cast<u8>(RegIndex::RAX))), -14);
    EXPECT_EQ(static_cast<i32>(m_cpu->gpr.get32(static_cast<u8>(RegIndex::RDX))), -2);
}

TEST_F(MulDivTest, IdivByZero_Exception) {
    // IDIV ECX with ECX=0 → #DE
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RAX), 100);
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RDX), 0);
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RCX), 0);
    u8 code[] = { 0xF7, 0xF9 };
    write_code(code, sizeof(code));

    Interpreter interp(*m_cpu, *m_memory);
    EXPECT_EQ(interp.step(), ExecResult::Exception);
}

TEST_F(MulDivTest, IdivRm8_Negative) {
    // F6 F9 = IDIV CL
    // AX = -100 (0xFF9C), CL = 7
    // -100 / 7 = -14, remainder = -2
    m_cpu->gpr.set16(static_cast<u8>(RegIndex::RAX), static_cast<u16>(static_cast<i16>(-100)));
    m_cpu->gpr.set8l(static_cast<u8>(RegIndex::RCX), 7);
    u8 code[] = { 0xF6, 0xF9 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(static_cast<i8>(m_cpu->gpr.get8l(static_cast<u8>(RegIndex::RAX))), -14);
    EXPECT_EQ(static_cast<i8>(m_cpu->gpr.get8h(0)), -2);
}

// ============================================================================
// CDQ/CQO/CBW Sign Extension Execution Tests
// ============================================================================

TEST_F(MulDivTest, Cdq_Positive) {
    // 99 = CDQ: sign-extend EAX to EDX:EAX
    // EAX = 0x7FFFFFFF (positive) → EDX = 0
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RAX), 0x7FFFFFFF);
    u8 code[] = { 0x99 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(static_cast<u8>(RegIndex::RAX)), 0x7FFFFFFFu);
    EXPECT_EQ(m_cpu->gpr.get32(static_cast<u8>(RegIndex::RDX)), 0x00000000u);
}

TEST_F(MulDivTest, Cdq_Negative) {
    // 99 = CDQ: EAX = 0x80000000 (negative) → EDX = 0xFFFFFFFF
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RAX), 0x80000000);
    u8 code[] = { 0x99 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(static_cast<u8>(RegIndex::RAX)), 0x80000000u);
    EXPECT_EQ(m_cpu->gpr.get32(static_cast<u8>(RegIndex::RDX)), 0xFFFFFFFFu);
}

TEST_F(MulDivTest, Cqo_Positive) {
    // 48 99 = CQO: sign-extend RAX to RDX:RAX
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RAX), 0x7FFFFFFFFFFFFFFFULL);
    u8 code[] = { 0x48, 0x99 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(static_cast<u8>(RegIndex::RAX)), 0x7FFFFFFFFFFFFFFFULL);
    EXPECT_EQ(m_cpu->gpr.get64(static_cast<u8>(RegIndex::RDX)), 0x0000000000000000ULL);
}

TEST_F(MulDivTest, Cqo_Negative) {
    // 48 99 = CQO: RAX = 0x8000000000000000 → RDX = 0xFFFFFFFFFFFFFFFF
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RAX), 0x8000000000000000ULL);
    u8 code[] = { 0x48, 0x99 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(static_cast<u8>(RegIndex::RAX)), 0x8000000000000000ULL);
    EXPECT_EQ(m_cpu->gpr.get64(static_cast<u8>(RegIndex::RDX)), 0xFFFFFFFFFFFFFFFFULL);
}

TEST_F(MulDivTest, Cbw_Positive) {
    // 66 98 = CBW: AX = sign-extend(AL)
    // AL=0x50 (positive) → AX=0x0050
    m_cpu->gpr.set8l(static_cast<u8>(RegIndex::RAX), 0x50);
    u8 code[] = { 0x66, 0x98 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get16(static_cast<u8>(RegIndex::RAX)), 0x0050u);
}

TEST_F(MulDivTest, Cbw_Negative) {
    // 66 98 = CBW: AL=0x80 (negative) → AX=0xFF80
    m_cpu->gpr.set8l(static_cast<u8>(RegIndex::RAX), 0x80);
    u8 code[] = { 0x66, 0x98 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get16(static_cast<u8>(RegIndex::RAX)), 0xFF80u);
}

TEST_F(MulDivTest, Cwde_Positive) {
    // 98 = CWDE: EAX = sign-extend(AX)
    m_cpu->gpr.set16(static_cast<u8>(RegIndex::RAX), 0x7FFF);
    u8 code[] = { 0x98 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(static_cast<u8>(RegIndex::RAX)), 0x00007FFFu);
}

TEST_F(MulDivTest, Cwde_Negative) {
    // 98 = CWDE: AX=0x8000 → EAX=0xFFFF8000
    m_cpu->gpr.set16(static_cast<u8>(RegIndex::RAX), 0x8000);
    u8 code[] = { 0x98 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(static_cast<u8>(RegIndex::RAX)), 0xFFFF8000u);
}

TEST_F(MulDivTest, Cdqe) {
    // 48 98 = CDQE: RAX = sign-extend(EAX)
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RAX), 0x80000000);
    u8 code[] = { 0x48, 0x98 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(static_cast<u8>(RegIndex::RAX)), 0xFFFFFFFF80000000ULL);
}

TEST_F(MulDivTest, Cwd_Negative) {
    // 66 99 = CWD: DX:AX = sign-extend(AX)
    // AX=0x8000 → DX=0xFFFF, AX unchanged
    m_cpu->gpr.set16(static_cast<u8>(RegIndex::RAX), 0x8000);
    u8 code[] = { 0x66, 0x99 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get16(static_cast<u8>(RegIndex::RAX)), 0x8000u);
    EXPECT_EQ(m_cpu->gpr.get16(static_cast<u8>(RegIndex::RDX)), 0xFFFFu);
}

// ============================================================================
// Flag Tests for MUL/IMUL
// ============================================================================

TEST_F(MulDivTest, MulRm8_FlagsClear) {
    // MUL CL: 5 * 3 = 15, fits in AL → CF=OF=0
    m_cpu->gpr.set8l(static_cast<u8>(RegIndex::RAX), 5);
    m_cpu->gpr.set8l(static_cast<u8>(RegIndex::RCX), 3);
    u8 code[] = { 0xF6, 0xE1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.of());
}

TEST_F(MulDivTest, MulRm8_FlagsSet) {
    // MUL CL: 128 * 2 = 256 = 0x100, doesn't fit in 8 bits → CF=OF=1
    m_cpu->gpr.set8l(static_cast<u8>(RegIndex::RAX), 128);
    m_cpu->gpr.set8l(static_cast<u8>(RegIndex::RCX), 2);
    u8 code[] = { 0xF6, 0xE1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get16(static_cast<u8>(RegIndex::RAX)), 256u);
    EXPECT_TRUE(m_cpu->flags.cf());
    EXPECT_TRUE(m_cpu->flags.of());
}

TEST_F(MulDivTest, ImulTwoOp_FlagsClear) {
    // IMUL EAX, ECX: 3 * 4 = 12, no overflow
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RAX), 3);
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RCX), 4);
    u8 code[] = { 0x0F, 0xAF, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(static_cast<u8>(RegIndex::RAX)), 12u);
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.of());
}

TEST_F(MulDivTest, ImulTwoOp_NegativeNoOverflow) {
    // IMUL EAX, ECX: -3 * 4 = -12, fits in i32
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RAX), static_cast<u32>(-3));
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RCX), 4);
    u8 code[] = { 0x0F, 0xAF, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(static_cast<i32>(m_cpu->gpr.get32(static_cast<u8>(RegIndex::RAX))), -12);
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.of());
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_F(MulDivTest, MulRm32_ByZero) {
    // MUL ECX: anything * 0 = 0, CF=OF=0
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RAX), 12345);
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RCX), 0);
    u8 code[] = { 0xF7, 0xE1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(static_cast<u8>(RegIndex::RAX)), 0u);
    EXPECT_EQ(m_cpu->gpr.get32(static_cast<u8>(RegIndex::RDX)), 0u);
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.of());
}

TEST_F(MulDivTest, DivRm8_Exact) {
    // DIV CL: AX=49, CL=7 → AL=7, AH=0
    m_cpu->gpr.set16(static_cast<u8>(RegIndex::RAX), 49);
    m_cpu->gpr.set8l(static_cast<u8>(RegIndex::RCX), 7);
    u8 code[] = { 0xF6, 0xF1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get8l(static_cast<u8>(RegIndex::RAX)), 7u);
    EXPECT_EQ(m_cpu->gpr.get8h(0), 0u);
}
