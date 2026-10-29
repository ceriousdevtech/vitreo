// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// CMP/TEST instruction tests - CPU-015

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
// CMP Decoder Tests
// ============================================================================

TEST(CmpTestDecoder, CmpRm32R32) {
    // 39 D8 - CMP EAX, EBX
    auto inst = decode({0x39, 0xD8});
    EXPECT_EQ(inst.opcode, Opcode::CMP_RM32_R32);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 0);  // EAX
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].reg, 3);  // EBX
}

TEST(CmpTestDecoder, CmpRm64R64) {
    // 48 39 D8 - CMP RAX, RBX
    auto inst = decode({0x48, 0x39, 0xD8});
    EXPECT_EQ(inst.opcode, Opcode::CMP_RM64_R64);
    EXPECT_EQ(inst.length, 3);
    EXPECT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 0);  // RAX
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[1].reg, 3);  // RBX
}

TEST(CmpTestDecoder, CmpAlImm8) {
    // 3C 42 - CMP AL, 0x42
    auto inst = decode({0x3C, 0x42});
    EXPECT_EQ(inst.opcode, Opcode::CMP_AL_IMM8);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[1].type, OperandType::Imm8);
    EXPECT_EQ(inst.operands[1].imm, 0x42);
}

TEST(CmpTestDecoder, CmpEaxImm32) {
    // 3D 78 56 34 12 - CMP EAX, 0x12345678
    auto inst = decode({0x3D, 0x78, 0x56, 0x34, 0x12});
    EXPECT_EQ(inst.opcode, Opcode::CMP_EAX_IMM32);
    EXPECT_EQ(inst.length, 5);
    EXPECT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].type, OperandType::Imm32);
    EXPECT_EQ(inst.operands[1].imm, 0x12345678);
}

TEST(CmpTestDecoder, CmpRm32Imm8) {
    // 83 F8 05 - CMP EAX, 5 (Group 1, /7, imm8)
    auto inst = decode({0x83, 0xF8, 0x05});
    EXPECT_EQ(inst.opcode, Opcode::CMP_RM32_IMM8);
    EXPECT_EQ(inst.length, 3);
    EXPECT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 0);  // EAX
    EXPECT_EQ(inst.operands[1].type, OperandType::Imm8);
    EXPECT_EQ(inst.operands[1].imm, 5);
}

// ============================================================================
// TEST Decoder Tests
// ============================================================================

TEST(CmpTestDecoder, TestRm32R32) {
    // 85 D8 - TEST EAX, EBX
    auto inst = decode({0x85, 0xD8});
    EXPECT_EQ(inst.opcode, Opcode::TEST_RM32_R32);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 0);  // EAX
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].reg, 3);  // EBX
}

TEST(CmpTestDecoder, TestRm64R64) {
    // 48 85 D8 - TEST RAX, RBX
    auto inst = decode({0x48, 0x85, 0xD8});
    EXPECT_EQ(inst.opcode, Opcode::TEST_RM64_R64);
    EXPECT_EQ(inst.length, 3);
    EXPECT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 0);  // RAX
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[1].reg, 3);  // RBX
}

TEST(CmpTestDecoder, TestAlImm8) {
    // A8 42 - TEST AL, 0x42
    auto inst = decode({0xA8, 0x42});
    EXPECT_EQ(inst.opcode, Opcode::TEST_AL_IMM8);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[1].type, OperandType::Imm8);
    EXPECT_EQ(inst.operands[1].imm, 0x42);
}

TEST(CmpTestDecoder, TestEaxImm32) {
    // A9 78 56 34 12 - TEST EAX, 0x12345678
    auto inst = decode({0xA9, 0x78, 0x56, 0x34, 0x12});
    EXPECT_EQ(inst.opcode, Opcode::TEST_EAX_IMM32);
    EXPECT_EQ(inst.length, 5);
    EXPECT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].type, OperandType::Imm32);
    EXPECT_EQ(inst.operands[1].imm, 0x12345678);
}

// ============================================================================
// Interpreter Tests
// ============================================================================

class CmpTestInterpreterTest : public ::testing::Test {
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
// CMP Interpreter Tests
// ============================================================================

TEST_F(CmpTestInterpreterTest, CmpEqual) {
    // CMP RAX, RBX where both are 5 → ZF=1, CF=0, SF=0
    m_cpu->gpr.set64(0, 5);  // RAX
    m_cpu->gpr.set64(3, 5);  // RBX

    // 48 39 D8 - CMP RAX, RBX
    u8 code[] = { 0x48, 0x39, 0xD8 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_TRUE(m_cpu->flags.zf());
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.sf());
}

TEST_F(CmpTestInterpreterTest, CmpLess) {
    // CMP RAX(3), RBX(5) → ZF=0, CF=1 (3<5 unsigned borrow), SF=1
    m_cpu->gpr.set64(0, 3);  // RAX
    m_cpu->gpr.set64(3, 5);  // RBX

    u8 code[] = { 0x48, 0x39, 0xD8 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_FALSE(m_cpu->flags.zf());
    EXPECT_TRUE(m_cpu->flags.cf());
    EXPECT_TRUE(m_cpu->flags.sf());
}

TEST_F(CmpTestInterpreterTest, CmpGreater) {
    // CMP RAX(5), RBX(3) → ZF=0, CF=0, SF=0
    m_cpu->gpr.set64(0, 5);  // RAX
    m_cpu->gpr.set64(3, 3);  // RBX

    u8 code[] = { 0x48, 0x39, 0xD8 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_FALSE(m_cpu->flags.zf());
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.sf());
}

TEST_F(CmpTestInterpreterTest, CmpDoesNotModifyOperands) {
    // After CMP, RAX and RBX must retain original values
    m_cpu->gpr.set64(0, 0x1234);  // RAX
    m_cpu->gpr.set64(3, 0x5678);  // RBX

    u8 code[] = { 0x48, 0x39, 0xD8 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0x1234);
    EXPECT_EQ(m_cpu->gpr.get64(3), 0x5678);
}

TEST_F(CmpTestInterpreterTest, CmpNegativeResult) {
    // CMP RAX(0), RBX(1) → result wraps, SF=1, CF=1
    m_cpu->gpr.set64(0, 0);
    m_cpu->gpr.set64(3, 1);

    u8 code[] = { 0x48, 0x39, 0xD8 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_TRUE(m_cpu->flags.sf());
    EXPECT_TRUE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.zf());
}

// ============================================================================
// TEST Interpreter Tests
// ============================================================================

TEST_F(CmpTestInterpreterTest, TestAlImm8Zero) {
    // TEST AL, 0 → ZF=1, SF=0, CF=0, OF=0
    m_cpu->gpr.set64(0, 0xFF);  // RAX (AL=0xFF)

    // A8 00 - TEST AL, 0x00
    u8 code[] = { 0xA8, 0x00 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_TRUE(m_cpu->flags.zf());
    EXPECT_FALSE(m_cpu->flags.sf());
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.of());
}

TEST_F(CmpTestInterpreterTest, TestNonZero) {
    // TEST RAX(0xFF), RBX(0x0F) → result=0x0F, ZF=0
    m_cpu->gpr.set64(0, 0xFF);
    m_cpu->gpr.set64(3, 0x0F);

    // 48 85 D8 - TEST RAX, RBX
    u8 code[] = { 0x48, 0x85, 0xD8 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_FALSE(m_cpu->flags.zf());
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.of());
}

TEST_F(CmpTestInterpreterTest, TestDoesNotModify) {
    // After TEST, RAX and RBX must retain original values
    m_cpu->gpr.set64(0, 0xAAAA);
    m_cpu->gpr.set64(3, 0x5555);

    u8 code[] = { 0x48, 0x85, 0xD8 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0xAAAA);
    EXPECT_EQ(m_cpu->gpr.get64(3), 0x5555);
}

TEST_F(CmpTestInterpreterTest, TestSetsSign) {
    // TEST with result having MSB set → SF=1
    m_cpu->gpr.set64(0, 0x8000000000000000ULL);
    m_cpu->gpr.set64(3, 0xFFFFFFFFFFFFFFFFULL);

    // 48 85 D8 - TEST RAX, RBX → AND = 0x8000000000000000
    u8 code[] = { 0x48, 0x85, 0xD8 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_TRUE(m_cpu->flags.sf());
    EXPECT_FALSE(m_cpu->flags.zf());
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.of());
}

// JIT records FlagOp lazily; interpreter CMP/TEST write rflags eagerly.
// exec_jcc rematerializes any pending FlagOp — if CMP does not retire it,
// a stale non-zero FlagOp makes equal CMP + JNE take the branch (observed
// skipping Spotify FullSetup extract when package version == 0x201).
TEST_F(CmpTestInterpreterTest, CmpRetiresStaleLazyFlagOpBeforeJne) {
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBP), 0x2000);
    ASSERT_TRUE(m_memory->write_u32(0x2000 - 0x19, 0x201u).is_ok());

    // Stale AND-shaped lazy op that would yield ZF=0 if rematerialized.
    m_cpu->flag_op = CpuState::FlagOp::And32;
    m_cpu->flag_src = 0xFFu;
    m_cpu->flag_dst = 0x0Fu;  // result 0x0F → ZF clear
    m_cpu->flags.set_zf(false);

    // 81 7D E7 01 02 00 00    cmp dword [rbp-0x19], 0x201
    // 75 10                   jne +0x10 (to 0x1019 — must NOT take)
    // 90                      nop (fallthrough at 0x1009)
    const u8 code[] = {
        0x81, 0x7D, 0xE7, 0x01, 0x02, 0x00, 0x00,
        0x75, 0x10,
        0x90,
    };
    write_code(code, sizeof(code));

    Interpreter interp(*m_cpu, *m_memory);
    EXPECT_EQ(interp.step(), ExecResult::Continue);  // CMP
    EXPECT_EQ(m_cpu->flag_op, CpuState::FlagOp::None);
    EXPECT_TRUE(m_cpu->flags.zf());
    EXPECT_EQ(m_cpu->rip, 0x1007u);

    EXPECT_EQ(interp.step(), ExecResult::Continue);  // JNE not taken
    EXPECT_EQ(m_cpu->rip, 0x1009u);                  // fallthrough to nop
}
