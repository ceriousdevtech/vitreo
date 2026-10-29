// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Bit manipulation instruction tests (BSF, BSR, BT, BTS, BTR, BTC, BSWAP) - CPU-025

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
// BSF/BSR Decoder Tests
// ============================================================================

TEST(BitOpsDecoder, BsfR32Rm32) {
    // 0F BC C1 - BSF EAX, ECX
    auto inst = decode({0x0F, 0xBC, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::BSF_R32_RM32);
    EXPECT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 0);  // EAX
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].reg, 1);  // ECX
}

TEST(BitOpsDecoder, BsrR32Rm32) {
    // 0F BD C1 - BSR EAX, ECX
    auto inst = decode({0x0F, 0xBD, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::BSR_R32_RM32);
    EXPECT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 0);  // EAX
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].reg, 1);  // ECX
}

TEST(BitOpsDecoder, BsrR64Rm64) {
    // 48 0F BD C1 - BSR RAX, RCX
    auto inst = decode({0x48, 0x0F, 0xBD, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::BSR_R64_RM64);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 0);  // RAX
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[1].reg, 1);  // RCX
}

TEST(BitOpsDecoder, TzcntR64Rm64) {
    // F3 48 0F BC C1 - TZCNT RAX, RCX
    auto inst = decode({0xF3, 0x48, 0x0F, 0xBC, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::TZCNT_R64_RM64);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg64);
}

TEST(BitOpsDecoder, LzcntR64Rm64) {
    // F3 48 0F BD C1 - LZCNT RAX, RCX
    auto inst = decode({0xF3, 0x48, 0x0F, 0xBD, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::LZCNT_R64_RM64);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg64);
}

TEST(BitOpsDecoder, BtRm32R32) {
    // 0F A3 C8 - BT EAX, ECX
    auto inst = decode({0x0F, 0xA3, 0xC8});
    EXPECT_EQ(inst.opcode, Opcode::BT_RM32_R32);
    EXPECT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 0);  // EAX
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].reg, 1);  // ECX
}

TEST(BitOpsDecoder, BtsRm32R32) {
    // 0F AB C8 - BTS EAX, ECX
    auto inst = decode({0x0F, 0xAB, 0xC8});
    EXPECT_EQ(inst.opcode, Opcode::BTS_RM32_R32);
}

TEST(BitOpsDecoder, BtrRm32R32) {
    // 0F B3 C8 - BTR EAX, ECX
    auto inst = decode({0x0F, 0xB3, 0xC8});
    EXPECT_EQ(inst.opcode, Opcode::BTR_RM32_R32);
}

TEST(BitOpsDecoder, BtcRm32R32) {
    // 0F BB C8 - BTC EAX, ECX
    auto inst = decode({0x0F, 0xBB, 0xC8});
    EXPECT_EQ(inst.opcode, Opcode::BTC_RM32_R32);
}

TEST(BitOpsDecoder, BtRm32Imm8) {
    // 0F BA E0 05 - BT EAX, 5
    auto inst = decode({0x0F, 0xBA, 0xE0, 0x05});
    EXPECT_EQ(inst.opcode, Opcode::BT_RM32_IMM8);
    EXPECT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 0);  // EAX
    EXPECT_EQ(inst.operands[1].type, OperandType::Imm8);
    EXPECT_EQ(inst.operands[1].imm, 5);
}

TEST(BitOpsDecoder, BswapR32) {
    // 0F C8 - BSWAP EAX
    auto inst = decode({0x0F, 0xC8});
    EXPECT_EQ(inst.opcode, Opcode::BSWAP_R32);
    EXPECT_EQ(inst.operand_count, 1);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 0);  // EAX
}

TEST(BitOpsDecoder, BswapR64) {
    // 48 0F C8 - BSWAP RAX
    auto inst = decode({0x48, 0x0F, 0xC8});
    EXPECT_EQ(inst.opcode, Opcode::BSWAP_R64);
    EXPECT_EQ(inst.operand_count, 1);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 0);  // RAX
}

// ============================================================================
// Interpreter Tests
// ============================================================================

class BitOpsInterpreterTest : public ::testing::Test {
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

// --- BSF execution tests ---

TEST_F(BitOpsInterpreterTest, BsfEaxEcx_Value0x10) {
    // 0F BC C1 - BSF EAX, ECX  (ECX=0x10 → EAX=4, ZF=0)
    m_cpu->gpr.set64(1, 0x10);  // ECX = 0x10
    m_cpu->gpr.set64(0, 0);     // EAX = 0
    u8 code[] = { 0x0F, 0xBC, 0xC1 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(0), 4u);   // Bit 4 is lowest set bit
    EXPECT_FALSE(m_cpu->flags.zf());
}

TEST_F(BitOpsInterpreterTest, BsfZeroSource_SetsZF) {
    // 0F BC C1 - BSF EAX, ECX  (ECX=0 → ZF=1)
    m_cpu->gpr.set64(1, 0);      // ECX = 0
    m_cpu->gpr.set64(0, 0x99);   // EAX = some value (should remain unchanged)
    u8 code[] = { 0x0F, 0xBC, 0xC1 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_TRUE(m_cpu->flags.zf());
    // Destination undefined but left unchanged per our implementation
    EXPECT_EQ(m_cpu->gpr.get32(0), 0x99u);
}

// --- BSR execution tests ---

TEST_F(BitOpsInterpreterTest, BsrEaxEcx_Value0x80) {
    // 0F BD C1 - BSR EAX, ECX  (ECX=0x80 → EAX=7)
    m_cpu->gpr.set64(1, 0x80);  // ECX = 0x80
    m_cpu->gpr.set64(0, 0);
    u8 code[] = { 0x0F, 0xBD, 0xC1 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(0), 7u);   // Bit 7 is highest set bit
    EXPECT_FALSE(m_cpu->flags.zf());
}

TEST_F(BitOpsInterpreterTest, BsrRaxRcx_64bit) {
    // 48 0F BD C1 - BSR RAX, RCX  (64-bit)
    m_cpu->gpr.set64(1, 0x100000000ULL);  // Bit 32 set
    m_cpu->gpr.set64(0, 0);
    u8 code[] = { 0x48, 0x0F, 0xBD, 0xC1 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 32u);
    EXPECT_FALSE(m_cpu->flags.zf());
}

TEST_F(BitOpsInterpreterTest, TzcntRaxRcx_64bit) {
    m_cpu->gpr.set64(1, 1ull << 40);
    m_cpu->gpr.set64(0, 0xAAAAAAAAAAAAAAAAull);
    u8 code[] = {0xF3, 0x48, 0x0F, 0xBC, 0xC1};
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 40u);
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.zf());
}

TEST_F(BitOpsInterpreterTest, LzcntRaxRcx_64bit) {
    m_cpu->gpr.set64(1, 1ull << 40);
    u8 code[] = {0xF3, 0x48, 0x0F, 0xBD, 0xC1};
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 23u);
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.zf());
}

TEST_F(BitOpsInterpreterTest, LzcntRaxRcx_ZeroReturns64AndSetsCF) {
    m_cpu->gpr.set64(1, 0);
    u8 code[] = {0xF3, 0x48, 0x0F, 0xBD, 0xC1};
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 64u);
    EXPECT_TRUE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.zf());
}

TEST_F(BitOpsInterpreterTest, TzcntAxCx_16bitPreservesUpperBits) {
    m_cpu->gpr.set64(1, 1u << 12);
    m_cpu->gpr.set64(0, 0xAABBCCDDEEFF0000ull);
    u8 code[] = {0x66, 0xF3, 0x0F, 0xBC, 0xC1};
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0xAABBCCDDEEFF000Cull);
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.zf());
}

// --- BT execution tests ---

TEST_F(BitOpsInterpreterTest, BtEaxEcx_BitSet) {
    // 0F A3 C8 - BT EAX, ECX  (test bit ECX in EAX)
    m_cpu->gpr.set64(0, 0x20);  // EAX = 0x20 (bit 5 set)
    m_cpu->gpr.set64(1, 5);     // ECX = 5 (test bit 5)
    u8 code[] = { 0x0F, 0xA3, 0xC8 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_TRUE(m_cpu->flags.cf());
}

TEST_F(BitOpsInterpreterTest, BtEaxEcx_BitClear) {
    // 0F A3 C8 - BT EAX, ECX  (test bit ECX in EAX)
    m_cpu->gpr.set64(0, 0x20);  // EAX = 0x20 (bit 5 set)
    m_cpu->gpr.set64(1, 3);     // ECX = 3 (test bit 3, which is clear)
    u8 code[] = { 0x0F, 0xA3, 0xC8 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_FALSE(m_cpu->flags.cf());
}

// --- BTS execution tests ---

TEST_F(BitOpsInterpreterTest, BtsEaxEcx_SetBit) {
    // 0F AB C8 - BTS EAX, ECX  (test and set bit ECX in EAX)
    m_cpu->gpr.set64(0, 0x00);  // EAX = 0
    m_cpu->gpr.set64(1, 3);     // ECX = 3
    u8 code[] = { 0x0F, 0xAB, 0xC8 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_FALSE(m_cpu->flags.cf());            // Original bit was 0
    EXPECT_EQ(m_cpu->gpr.get32(0), 0x08u);     // Bit 3 now set
}

// --- BTR execution tests ---

TEST_F(BitOpsInterpreterTest, BtrEaxEcx_ResetBit) {
    // 0F B3 C8 - BTR EAX, ECX  (test and reset bit ECX in EAX)
    m_cpu->gpr.set64(0, 0xFF);  // EAX = 0xFF
    m_cpu->gpr.set64(1, 3);     // ECX = 3
    u8 code[] = { 0x0F, 0xB3, 0xC8 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_TRUE(m_cpu->flags.cf());             // Original bit was 1
    EXPECT_EQ(m_cpu->gpr.get32(0), 0xF7u);     // Bit 3 now clear
}

// --- BTC execution tests ---

TEST_F(BitOpsInterpreterTest, BtcEaxEcx_ComplementBit) {
    // 0F BB C8 - BTC EAX, ECX  (test and complement bit ECX in EAX)
    m_cpu->gpr.set64(0, 0x00);  // EAX = 0
    m_cpu->gpr.set64(1, 5);     // ECX = 5
    u8 code[] = { 0x0F, 0xBB, 0xC8 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_FALSE(m_cpu->flags.cf());            // Original bit was 0
    EXPECT_EQ(m_cpu->gpr.get32(0), 0x20u);     // Bit 5 now set (complemented)
}

// --- BT imm8 execution tests ---

TEST_F(BitOpsInterpreterTest, BtEaxImm8_BitSet) {
    // 0F BA E0 05 - BT EAX, 5
    m_cpu->gpr.set64(0, 0x20);  // EAX = 0x20 (bit 5 set)
    u8 code[] = { 0x0F, 0xBA, 0xE0, 0x05 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_TRUE(m_cpu->flags.cf());
}

TEST_F(BitOpsInterpreterTest, BtEaxImm8_BitClear) {
    // 0F BA E0 03 - BT EAX, 3
    m_cpu->gpr.set64(0, 0x20);  // EAX = 0x20 (bit 3 not set)
    u8 code[] = { 0x0F, 0xBA, 0xE0, 0x03 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_FALSE(m_cpu->flags.cf());
}

// --- BSWAP execution tests ---

TEST_F(BitOpsInterpreterTest, BswapEax_32bit) {
    // 0F C8 - BSWAP EAX  (EAX=0x12345678 → 0x78563412)
    m_cpu->gpr.set64(0, 0x12345678);
    u8 code[] = { 0x0F, 0xC8 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(0), 0x78563412u);
}

TEST_F(BitOpsInterpreterTest, BswapRax_64bit) {
    // 48 0F C8 - BSWAP RAX  (64-bit)
    m_cpu->gpr.set64(0, 0x0102030405060708ULL);
    u8 code[] = { 0x48, 0x0F, 0xC8 };
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0x0807060504030201ULL);
}
