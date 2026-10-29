// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JMP/Jcc instruction tests - CPU-014

#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include <gtest/gtest.h>

using namespace vitreo;
using namespace vitreo::cpu;

// ============================================================================
// Decode helper
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
// JMP Decoder Tests
// ============================================================================

TEST(JumpDecoder, JmpRel8) {
    // EB 10 - JMP short +0x10
    auto inst = decode({0xEB, 0x10});
    EXPECT_EQ(inst.opcode, Opcode::JMP_REL8);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operand_count, 1);
    EXPECT_EQ(inst.operands[0].type, OperandType::Rel8);
    EXPECT_EQ(inst.operands[0].imm, 0x10);
}

TEST(JumpDecoder, JmpRel32) {
    // E9 00 10 00 00 - JMP near +0x1000
    auto inst = decode({0xE9, 0x00, 0x10, 0x00, 0x00});
    EXPECT_EQ(inst.opcode, Opcode::JMP_REL32);
    EXPECT_EQ(inst.length, 5);
    EXPECT_EQ(inst.operand_count, 1);
    EXPECT_EQ(inst.operands[0].type, OperandType::Rel32);
    EXPECT_EQ(inst.operands[0].imm, 0x1000);
}

// ============================================================================
// Jcc rel8 Decoder Tests (0x70-0x7F)
// ============================================================================

TEST(JumpDecoder, JccRel8_JZ) {
    // 74 10 - JZ +0x10
    auto inst = decode({0x74, 0x10});
    EXPECT_EQ(inst.opcode, Opcode::JZ_REL8);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Rel8);
    EXPECT_EQ(inst.operands[0].imm, 0x10);
}

TEST(JumpDecoder, JccRel8_JNZ) {
    // 75 10 - JNZ +0x10
    auto inst = decode({0x75, 0x10});
    EXPECT_EQ(inst.opcode, Opcode::JNZ_REL8);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Rel8);
    EXPECT_EQ(inst.operands[0].imm, 0x10);
}

TEST(JumpDecoder, JccRel8_JB) {
    // 72 10 - JB +0x10
    auto inst = decode({0x72, 0x10});
    EXPECT_EQ(inst.opcode, Opcode::JB_REL8);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Rel8);
    EXPECT_EQ(inst.operands[0].imm, 0x10);
}

TEST(JumpDecoder, JccRel8_JNB) {
    // 73 10 - JNB/JAE +0x10
    auto inst = decode({0x73, 0x10});
    EXPECT_EQ(inst.opcode, Opcode::JNB_REL8);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Rel8);
    EXPECT_EQ(inst.operands[0].imm, 0x10);
}

TEST(JumpDecoder, JccRel8_JO) {
    // 70 10 - JO +0x10
    auto inst = decode({0x70, 0x10});
    EXPECT_EQ(inst.opcode, Opcode::JO_REL8);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Rel8);
    EXPECT_EQ(inst.operands[0].imm, 0x10);
}

// ============================================================================
// LOOP / JCXZ-family Decoder Tests (E0-E3)
// ============================================================================

TEST(JumpDecoder, LoopRel8) {
    // E2 FE - LOOP -2
    auto inst = decode({0xE2, 0xFE});
    EXPECT_EQ(inst.opcode, Opcode::LOOP_REL8);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operand_count, 1);
    EXPECT_EQ(inst.operands[0].type, OperandType::Rel8);
    EXPECT_EQ(inst.operands[0].imm, -2);
}

TEST(JumpDecoder, JrcxzRel8_Mode64) {
    // E3 05 - JRCXZ +5 (64-bit mode)
    auto inst = decode({0xE3, 0x05}, DecodeMode::Mode64);
    EXPECT_EQ(inst.opcode, Opcode::JRCXZ_REL8);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operand_count, 1);
    EXPECT_EQ(inst.operands[0].type, OperandType::Rel8);
    EXPECT_EQ(inst.operands[0].imm, 5);
}

TEST(JumpDecoder, JccRel8_JNO) {
    // 71 10 - JNO +0x10
    auto inst = decode({0x71, 0x10});
    EXPECT_EQ(inst.opcode, Opcode::JNO_REL8);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Rel8);
    EXPECT_EQ(inst.operands[0].imm, 0x10);
}

TEST(JumpDecoder, JccRel8_JBE) {
    // 76 10 - JBE +0x10
    auto inst = decode({0x76, 0x10});
    EXPECT_EQ(inst.opcode, Opcode::JBE_REL8);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Rel8);
    EXPECT_EQ(inst.operands[0].imm, 0x10);
}

TEST(JumpDecoder, JccRel8_JNBE) {
    // 77 10 - JA/JNBE +0x10
    auto inst = decode({0x77, 0x10});
    EXPECT_EQ(inst.opcode, Opcode::JNBE_REL8);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Rel8);
    EXPECT_EQ(inst.operands[0].imm, 0x10);
}

TEST(JumpDecoder, JccRel8_JS) {
    // 78 10 - JS +0x10
    auto inst = decode({0x78, 0x10});
    EXPECT_EQ(inst.opcode, Opcode::JS_REL8);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Rel8);
    EXPECT_EQ(inst.operands[0].imm, 0x10);
}

TEST(JumpDecoder, JccRel8_JNS) {
    // 79 10 - JNS +0x10
    auto inst = decode({0x79, 0x10});
    EXPECT_EQ(inst.opcode, Opcode::JNS_REL8);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Rel8);
    EXPECT_EQ(inst.operands[0].imm, 0x10);
}

TEST(JumpDecoder, JccRel8_JP) {
    // 7A 10 - JP +0x10
    auto inst = decode({0x7A, 0x10});
    EXPECT_EQ(inst.opcode, Opcode::JP_REL8);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Rel8);
    EXPECT_EQ(inst.operands[0].imm, 0x10);
}

TEST(JumpDecoder, JccRel8_JNP) {
    // 7B 10 - JNP +0x10
    auto inst = decode({0x7B, 0x10});
    EXPECT_EQ(inst.opcode, Opcode::JNP_REL8);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Rel8);
    EXPECT_EQ(inst.operands[0].imm, 0x10);
}

TEST(JumpDecoder, JccRel8_JL) {
    // 7C 10 - JL +0x10
    auto inst = decode({0x7C, 0x10});
    EXPECT_EQ(inst.opcode, Opcode::JL_REL8);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Rel8);
    EXPECT_EQ(inst.operands[0].imm, 0x10);
}

TEST(JumpDecoder, JccRel8_JGE) {
    // 7D 10 - JNL/JGE +0x10
    auto inst = decode({0x7D, 0x10});
    EXPECT_EQ(inst.opcode, Opcode::JNL_REL8);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Rel8);
    EXPECT_EQ(inst.operands[0].imm, 0x10);
}

TEST(JumpDecoder, JccRel8_JLE) {
    // 7E 10 - JLE +0x10
    auto inst = decode({0x7E, 0x10});
    EXPECT_EQ(inst.opcode, Opcode::JLE_REL8);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Rel8);
    EXPECT_EQ(inst.operands[0].imm, 0x10);
}

TEST(JumpDecoder, JccRel8_JG) {
    // 7F 10 - JNLE/JG +0x10
    auto inst = decode({0x7F, 0x10});
    EXPECT_EQ(inst.opcode, Opcode::JNLE_REL8);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Rel8);
    EXPECT_EQ(inst.operands[0].imm, 0x10);
}

// ============================================================================
// Jcc rel32 Decoder Tests (0F 80-8F)
// ============================================================================

TEST(JumpDecoder, JccRel32_JZ) {
    // 0F 84 10 00 00 00 - JZ +0x10
    auto inst = decode({0x0F, 0x84, 0x10, 0x00, 0x00, 0x00});
    EXPECT_EQ(inst.opcode, Opcode::JZ_REL32);
    EXPECT_EQ(inst.length, 6);
    EXPECT_EQ(inst.operands[0].type, OperandType::Rel32);
    EXPECT_EQ(inst.operands[0].imm, 0x10);
}

TEST(JumpDecoder, JccRel32_JNZ) {
    // 0F 85 10 00 00 00 - JNZ +0x10
    auto inst = decode({0x0F, 0x85, 0x10, 0x00, 0x00, 0x00});
    EXPECT_EQ(inst.opcode, Opcode::JNZ_REL32);
    EXPECT_EQ(inst.length, 6);
    EXPECT_EQ(inst.operands[0].type, OperandType::Rel32);
    EXPECT_EQ(inst.operands[0].imm, 0x10);
}

TEST(JumpDecoder, JccRel32_JB) {
    // 0F 82 10 00 00 00 - JB +0x10
    auto inst = decode({0x0F, 0x82, 0x10, 0x00, 0x00, 0x00});
    EXPECT_EQ(inst.opcode, Opcode::JB_REL32);
    EXPECT_EQ(inst.length, 6);
    EXPECT_EQ(inst.operands[0].type, OperandType::Rel32);
    EXPECT_EQ(inst.operands[0].imm, 0x10);
}

// ============================================================================
// Interpreter Test Fixture
// ============================================================================

class JumpInterpreterTest : public ::testing::Test {
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
// JMP Interpreter Tests
// ============================================================================

TEST_F(JumpInterpreterTest, JmpRel8_Forward) {
    // EB 10 - JMP +0x10  (2-byte instruction)
    // rip = 0x1000 + 2 + 0x10 = 0x1012
    u8 code[] = {0xEB, 0x10};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x1012u);
}

TEST_F(JumpInterpreterTest, JmpRel8_Backward) {
    // EB FE - JMP -2  (infinite loop: rip = 0x1000 + 2 + (-2) = 0x1000)
    u8 code[] = {0xEB, 0xFE};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x1000u);
}

// ============================================================================
// LOOP / JCXZ-family Interpreter Tests
// ============================================================================

TEST_F(JumpInterpreterTest, LoopRel8_DecrementsRcxAndBranches) {
    // loop:
    //   E2 FE - LOOP -2
    //   F4    - HLT
    // Start RCX=2: branches once back to 0x1000, then falls through to HLT.
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RCX), 2);
    u8 code[] = {0xE2, 0xFE, 0xF4};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x1000u);
    EXPECT_EQ(m_cpu->gpr.get64(static_cast<u8>(RegIndex::RCX)), 1u);

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(static_cast<u8>(RegIndex::RCX)), 0u);

    EXPECT_EQ(execute_one(), ExecResult::Exception);
}

TEST_F(JumpInterpreterTest, JrcxzTakenWhenZero) {
    // E3 01 - JRCXZ +1 (to skip HLT)
    // F4    - HLT
    // 90    - NOP
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RCX), 0);
    u8 code[] = {0xE3, 0x01, 0xF4, 0x90};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x1003u);
}

// ============================================================================
// JZ / JNZ Interpreter Tests
// ============================================================================

TEST_F(JumpInterpreterTest, JzTaken) {
    // 74 10 - JZ +0x10  (ZF=1 → taken)
    m_cpu->flags.set_zf(true);
    u8 code[] = {0x74, 0x10};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x1012u);
}

TEST_F(JumpInterpreterTest, JzNotTaken) {
    // 74 10 - JZ +0x10  (ZF=0 → not taken)
    m_cpu->flags.set_zf(false);
    u8 code[] = {0x74, 0x10};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
}

TEST_F(JumpInterpreterTest, JnzTaken) {
    // 75 10 - JNZ +0x10  (ZF=0 → taken)
    m_cpu->flags.set_zf(false);
    u8 code[] = {0x75, 0x10};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x1012u);
}

TEST_F(JumpInterpreterTest, JnzNotTaken) {
    // 75 10 - JNZ +0x10  (ZF=1 → not taken)
    m_cpu->flags.set_zf(true);
    u8 code[] = {0x75, 0x10};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
}

// ============================================================================
// JB / JNB (CF) Interpreter Tests
// ============================================================================

TEST_F(JumpInterpreterTest, JbTaken) {
    // 72 10 - JB +0x10  (CF=1 → taken)
    m_cpu->flags.set_cf(true);
    u8 code[] = {0x72, 0x10};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x1012u);
}

TEST_F(JumpInterpreterTest, JbNotTaken) {
    // 72 10 - JB +0x10  (CF=0 → not taken)
    m_cpu->flags.set_cf(false);
    u8 code[] = {0x72, 0x10};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
}

TEST_F(JumpInterpreterTest, JnbTaken) {
    // 73 10 - JNB/JAE +0x10  (CF=0 → taken)
    m_cpu->flags.set_cf(false);
    u8 code[] = {0x73, 0x10};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x1012u);
}

// ============================================================================
// JBE / JA (CF||ZF) Interpreter Tests
// ============================================================================

TEST_F(JumpInterpreterTest, JbeTaken_CarrySet) {
    // 76 10 - JBE +0x10  (CF=1, ZF=0 → taken)
    m_cpu->flags.set_cf(true);
    m_cpu->flags.set_zf(false);
    u8 code[] = {0x76, 0x10};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x1012u);
}

TEST_F(JumpInterpreterTest, JbeTaken_ZeroSet) {
    // 76 10 - JBE +0x10  (CF=0, ZF=1 → taken)
    m_cpu->flags.set_cf(false);
    m_cpu->flags.set_zf(true);
    u8 code[] = {0x76, 0x10};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x1012u);
}

TEST_F(JumpInterpreterTest, JbeNotTaken) {
    // 76 10 - JBE +0x10  (CF=0, ZF=0 → not taken)
    m_cpu->flags.set_cf(false);
    m_cpu->flags.set_zf(false);
    u8 code[] = {0x76, 0x10};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
}

TEST_F(JumpInterpreterTest, JaTaken) {
    // 77 10 - JA/JNBE +0x10  (CF=0, ZF=0 → taken)
    m_cpu->flags.set_cf(false);
    m_cpu->flags.set_zf(false);
    u8 code[] = {0x77, 0x10};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x1012u);
}

// ============================================================================
// JS / JNS (SF) Interpreter Tests
// ============================================================================

TEST_F(JumpInterpreterTest, JsTaken) {
    // 78 10 - JS +0x10  (SF=1 → taken)
    m_cpu->flags.set_sf(true);
    u8 code[] = {0x78, 0x10};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x1012u);
}

TEST_F(JumpInterpreterTest, JnsNotTaken) {
    // 79 10 - JNS +0x10  (SF=1 → not taken)
    m_cpu->flags.set_sf(true);
    u8 code[] = {0x79, 0x10};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
}

// ============================================================================
// JP / JNP (PF) Interpreter Tests
// ============================================================================

TEST_F(JumpInterpreterTest, JpTaken) {
    // 7A 10 - JP +0x10  (PF=1 → taken)
    m_cpu->flags.set_pf(true);
    u8 code[] = {0x7A, 0x10};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x1012u);
}

// ============================================================================
// JO / JNO (OF) Interpreter Tests
// ============================================================================

TEST_F(JumpInterpreterTest, JoTaken) {
    // 70 10 - JO +0x10  (OF=1 → taken)
    m_cpu->flags.set_of(true);
    u8 code[] = {0x70, 0x10};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x1012u);
}

// ============================================================================
// JL / JGE (SF!=OF / SF==OF) Interpreter Tests
// ============================================================================

TEST_F(JumpInterpreterTest, JlTaken) {
    // 7C 10 - JL +0x10  (SF!=OF: SF=1, OF=0 → taken)
    m_cpu->flags.set_sf(true);
    m_cpu->flags.set_of(false);
    u8 code[] = {0x7C, 0x10};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x1012u);
}

TEST_F(JumpInterpreterTest, JlNotTaken) {
    // 7C 10 - JL +0x10  (SF==OF: SF=1, OF=1 → not taken)
    m_cpu->flags.set_sf(true);
    m_cpu->flags.set_of(true);
    u8 code[] = {0x7C, 0x10};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
}

TEST_F(JumpInterpreterTest, JgeTaken) {
    // 7D 10 - JGE/JNL +0x10  (SF==OF: SF=0, OF=0 → taken)
    m_cpu->flags.set_sf(false);
    m_cpu->flags.set_of(false);
    u8 code[] = {0x7D, 0x10};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x1012u);
}

// ============================================================================
// JLE / JG (ZF==1||SF!=OF / ZF==0&&SF==OF) Interpreter Tests
// ============================================================================

TEST_F(JumpInterpreterTest, JleTaken_ZeroSet) {
    // 7E 10 - JLE +0x10  (ZF=1 → taken regardless of SF/OF)
    m_cpu->flags.set_zf(true);
    m_cpu->flags.set_sf(false);
    m_cpu->flags.set_of(false);
    u8 code[] = {0x7E, 0x10};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x1012u);
}

TEST_F(JumpInterpreterTest, JleTaken_SignNotEqualOverflow) {
    // 7E 10 - JLE +0x10  (ZF=0, SF!=OF: SF=1, OF=0 → taken)
    m_cpu->flags.set_zf(false);
    m_cpu->flags.set_sf(true);
    m_cpu->flags.set_of(false);
    u8 code[] = {0x7E, 0x10};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x1012u);
}

TEST_F(JumpInterpreterTest, JleNotTaken) {
    // 7E 10 - JLE +0x10  (ZF=0, SF==OF: SF=0, OF=0 → not taken)
    m_cpu->flags.set_zf(false);
    m_cpu->flags.set_sf(false);
    m_cpu->flags.set_of(false);
    u8 code[] = {0x7E, 0x10};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
}

TEST_F(JumpInterpreterTest, JgTaken) {
    // 7F 10 - JG/JNLE +0x10  (ZF=0, SF==OF: SF=0, OF=0 → taken)
    m_cpu->flags.set_zf(false);
    m_cpu->flags.set_sf(false);
    m_cpu->flags.set_of(false);
    u8 code[] = {0x7F, 0x10};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x1012u);
}

TEST_F(JumpInterpreterTest, JgNotTaken_ZeroSet) {
    // 7F 10 - JG/JNLE +0x10  (ZF=1 → not taken)
    m_cpu->flags.set_zf(true);
    m_cpu->flags.set_sf(false);
    m_cpu->flags.set_of(false);
    u8 code[] = {0x7F, 0x10};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
}

// ============================================================================
// Jcc rel32 Interpreter Test
// ============================================================================

TEST_F(JumpInterpreterTest, JzRel32Taken) {
    // 0F 84 10 00 00 00 - JZ +0x10 (6-byte instruction)
    // rip = 0x1000 + 6 + 0x10 = 0x1016
    m_cpu->flags.set_zf(true);
    u8 code[] = {0x0F, 0x84, 0x10, 0x00, 0x00, 0x00};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x1016u);
}

// ============================================================================
// Lazy-flag hand-off from the JIT
//
// The JIT defers flag computation into CpuState::flag_op/flag_src/flag_dst and
// only settles it inside its own condition helpers. If a block exits before the
// consumer runs (self-modifying guest code, interpreter fallback, thunk return)
// the interpreter must settle the pending op itself rather than reading the
// stale rflags image left by an older instruction.
// ============================================================================

namespace {

/// Record the lazy state the JIT would leave behind for `CMP a, b`, and poison
/// the architectural EFLAGS so a stale read is unambiguously visible.
void stage_pending_cmp32(CpuState& cpu, u32 a, u32 b) {
    cpu.flags.rflags = 0;
    cpu.flags.set_zf(a != b);   // deliberately inverted vs. the real result
    cpu.flags.set_cf(a >= b);
    cpu.flag_op = CpuState::FlagOp::Sub32;
    cpu.flag_src = b;
    cpu.flag_dst = static_cast<u32>(a - b);
}

} // namespace

TEST_F(JumpInterpreterTest, JzObservesPendingLazyCompare) {
    // 74 10 - JZ +0x10. Pending CMP 5,3 is not equal, so the branch must fall
    // through even though the stale EFLAGS image has ZF=1.
    stage_pending_cmp32(*m_cpu, 5, 3);
    u8 code[] = {0x74, 0x10};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->flag_op, CpuState::FlagOp::None);
}

TEST_F(JumpInterpreterTest, JbObservesPendingLazyCompare) {
    // 72 10 - JB +0x10. Pending CMP 3,5 borrows, so CF=1 and the branch is
    // taken even though the stale EFLAGS image has CF=0.
    stage_pending_cmp32(*m_cpu, 3, 5);
    u8 code[] = {0x72, 0x10};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x1012u);
}

TEST_F(JumpInterpreterTest, LoopeObservesPendingLazyCompare) {
    // E1 FE - LOOPE -2. Pending CMP 5,3 clears ZF, so the loop must exit even
    // though RCX is still non-zero and the stale image has ZF=1.
    stage_pending_cmp32(*m_cpu, 5, 3);
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RCX), 4);
    u8 code[] = {0xE1, 0xFE};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(static_cast<u8>(RegIndex::RCX)), 3u);
}
