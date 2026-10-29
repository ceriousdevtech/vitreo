// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// INC/DEC/NEG/ADC/SBB tests (CPU-020)

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
// Test Fixture
// ============================================================================

class IncDecAdcTest : public ::testing::Test {
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

    // Execute bytes directly with current cpu/mem state
    ExecResult execute_bytes(std::initializer_list<u8> bytes) {
        u64 addr = m_cpu->rip;
        for (u8 b : bytes) {
            m_memory->write_u8(addr++, b);
        }
        Interpreter interp(*m_cpu, *m_memory);
        return interp.step();
    }

    std::unique_ptr<GuestMemory> m_memory;
    std::unique_ptr<CpuState> m_cpu;
};

// ============================================================================
// Decode Tests — ADC
// ============================================================================

TEST(IncDecAdcDecoder, AdcRm8R8) {
    // 10 D8 — ADC AL, BL
    auto inst = decode({0x10, 0xD8});
    EXPECT_EQ(inst.opcode, Opcode::ADC_RM8_R8);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[0].reg, 0);  // AL
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[1].reg, 3);  // BL
}

TEST(IncDecAdcDecoder, AdcRm32R32) {
    // 11 C8 — ADC EAX, ECX
    auto inst = decode({0x11, 0xC8});
    EXPECT_EQ(inst.opcode, Opcode::ADC_RM32_R32);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 0);  // EAX
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].reg, 1);  // ECX
}

TEST(IncDecAdcDecoder, AdcR32Rm32) {
    // 13 C1 — ADC EAX, ECX
    auto inst = decode({0x13, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::ADC_R32_RM32);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 0);  // EAX
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].reg, 1);  // ECX
}

TEST(IncDecAdcDecoder, AdcAlImm8) {
    // 14 05 — ADC AL, 5
    auto inst = decode({0x14, 0x05});
    EXPECT_EQ(inst.opcode, Opcode::ADC_AL_IMM8);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[0].reg, 0);
    EXPECT_EQ(inst.operands[1].imm, 5);
}

TEST(IncDecAdcDecoder, AdcEaxImm32) {
    // 15 78 56 34 12 — ADC EAX, 0x12345678
    auto inst = decode({0x15, 0x78, 0x56, 0x34, 0x12});
    EXPECT_EQ(inst.opcode, Opcode::ADC_EAX_IMM32);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].imm, 0x12345678);
}

TEST(IncDecAdcDecoder, AdcRaxImm32) {
    // 48 15 78 56 34 12 — ADC RAX, 0x12345678
    auto inst = decode({0x48, 0x15, 0x78, 0x56, 0x34, 0x12});
    EXPECT_EQ(inst.opcode, Opcode::ADC_RAX_IMM32);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
}

// ============================================================================
// Decode Tests — SBB
// ============================================================================

TEST(IncDecAdcDecoder, SbbRm32R32) {
    // 19 C8 — SBB EAX, ECX
    auto inst = decode({0x19, 0xC8});
    EXPECT_EQ(inst.opcode, Opcode::SBB_RM32_R32);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 0);  // EAX
}

TEST(IncDecAdcDecoder, SbbR32Rm32) {
    // 1B C1 — SBB EAX, ECX
    auto inst = decode({0x1B, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::SBB_R32_RM32);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 0);
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].reg, 1);
}

TEST(IncDecAdcDecoder, SbbAlImm8) {
    // 1C 03 — SBB AL, 3
    auto inst = decode({0x1C, 0x03});
    EXPECT_EQ(inst.opcode, Opcode::SBB_AL_IMM8);
    EXPECT_EQ(inst.operands[1].imm, 3);
}

TEST(IncDecAdcDecoder, SbbEaxImm32) {
    // 1D 10 00 00 00 — SBB EAX, 0x10
    auto inst = decode({0x1D, 0x10, 0x00, 0x00, 0x00});
    EXPECT_EQ(inst.opcode, Opcode::SBB_EAX_IMM32);
    EXPECT_EQ(inst.operands[1].imm, 0x10);
}

TEST(IncDecAdcDecoder, SbbRaxImm32) {
    // 48 1D 78 56 34 12 — SBB RAX, 0x12345678
    auto inst = decode({0x48, 0x1D, 0x78, 0x56, 0x34, 0x12});
    EXPECT_EQ(inst.opcode, Opcode::SBB_RAX_IMM32);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[1].imm, 0x12345678);
}

TEST(IncDecAdcDecoder, SbbRm64Imm32) {
    // 48 81 D9 EF BE AD 0E — SBB RCX, imm32
    auto inst = decode({0x48, 0x81, 0xD9, 0xEF, 0xBE, 0xAD, 0x0E});
    EXPECT_EQ(inst.opcode, Opcode::SBB_RM64_IMM32);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 1);  // RCX
}

// ============================================================================
// Decode Tests — Group 4 (FE)
// ============================================================================

TEST(IncDecAdcDecoder, Group4_IncRm8) {
    // FE C1 — INC CL
    auto inst = decode({0xFE, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::INC_RM8);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[0].reg, 1);  // CL
}

TEST(IncDecAdcDecoder, Group4_DecRm8) {
    // FE C9 — DEC CL
    auto inst = decode({0xFE, 0xC9});
    EXPECT_EQ(inst.opcode, Opcode::DEC_RM8);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[0].reg, 1);  // CL
}

// ============================================================================
// Decode Tests — Group 5 (FF) INC/DEC
// ============================================================================

TEST(IncDecAdcDecoder, Group5_IncRm32) {
    // FF C1 — INC ECX
    auto inst = decode({0xFF, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::INC_RM32);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 1);  // ECX
}

TEST(IncDecAdcDecoder, Group5_IncRm64) {
    // 48 FF C1 — INC RCX
    auto inst = decode({0x48, 0xFF, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::INC_RM64);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 1);  // RCX
}

TEST(IncDecAdcDecoder, Group5_DecRm32) {
    // FF C9 — DEC ECX
    auto inst = decode({0xFF, 0xC9});
    EXPECT_EQ(inst.opcode, Opcode::DEC_RM32);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 1);  // ECX
}

TEST(IncDecAdcDecoder, Group5_DecRm64) {
    // 48 FF C9 — DEC RCX
    auto inst = decode({0x48, 0xFF, 0xC9});
    EXPECT_EQ(inst.opcode, Opcode::DEC_RM64);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 1);  // RCX
}

// ============================================================================
// Group 1 ADC/SBB immediate decode tests
// ============================================================================

TEST(IncDecAdcDecoder, Group1_AdcRm32Imm8) {
    // 83 D0 05 — ADC EAX, 5  (/2)
    auto inst = decode({0x83, 0xD0, 0x05});
    EXPECT_EQ(inst.opcode, Opcode::ADC_RM32_IMM8);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 0);  // EAX
    EXPECT_EQ(inst.operands[1].imm, 5);
}

TEST(IncDecAdcDecoder, Group1_SbbRm64Imm8) {
    // 48 83 D8 03 — SBB RAX, 3  (/3)
    auto inst = decode({0x48, 0x83, 0xD8, 0x03});
    EXPECT_EQ(inst.opcode, Opcode::SBB_RM64_IMM8);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 0);  // RAX
    EXPECT_EQ(inst.operands[1].imm, 3);
}

// ============================================================================
// Interpreter Tests — INC
// ============================================================================

TEST_F(IncDecAdcTest, IncEcx_BasicValue) {
    // FF C1 — INC ECX
    m_cpu->gpr.set32(1, 5);  // ECX = 5
    m_cpu->flags.set_cf(true);  // CF should be preserved

    u8 code[] = { 0xFF, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(1), 6u);
    EXPECT_TRUE(m_cpu->flags.cf());   // CF preserved
    EXPECT_FALSE(m_cpu->flags.zf());
    EXPECT_FALSE(m_cpu->flags.sf());
}

TEST_F(IncDecAdcTest, IncRm8_Reg) {
    // FE C1 — INC CL
    m_cpu->gpr.set8l(1, 0x7F);  // CL = 127

    u8 code[] = { 0xFE, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get8l(1), 0x80);
    EXPECT_TRUE(m_cpu->flags.of());   // Signed overflow 127→-128
    EXPECT_TRUE(m_cpu->flags.sf());
}

TEST_F(IncDecAdcTest, IncOverflow32_SetsZero) {
    // FF C1 — INC ECX  with ECX = 0xFFFFFFFF
    m_cpu->gpr.set32(1, 0xFFFFFFFF);
    m_cpu->flags.set_cf(false);  // Original CF=0

    u8 code[] = { 0xFF, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(1), 0u);
    EXPECT_TRUE(m_cpu->flags.zf());    // Result is zero
    EXPECT_FALSE(m_cpu->flags.cf());   // CF unchanged (still 0)
}

TEST_F(IncDecAdcTest, AdcHighBytePropagatesLowByteCarry) {
    m_cpu->gpr.set32(0, 0x00000002);  // AL = 2
    m_cpu->gpr.set32(1, 0x1234fffe);  // CH = 0xff, CL = 0xfe

    EXPECT_EQ(execute_bytes({0x02, 0xc8}), ExecResult::Continue);  // ADD CL, AL
    EXPECT_EQ(m_cpu->gpr.get32(1), 0x1234ff00u);
    EXPECT_TRUE(m_cpu->flags.cf());

    EXPECT_EQ(execute_bytes({0x80, 0xd5, 0x00}), ExecResult::Continue);  // ADC CH, 0
    EXPECT_EQ(m_cpu->gpr.get32(1), 0x12340000u);
    EXPECT_TRUE(m_cpu->flags.cf());
}

// ============================================================================
// Interpreter Tests — DEC
// ============================================================================

TEST_F(IncDecAdcTest, DecEcx_ToZero) {
    // FF C9 — DEC ECX
    m_cpu->gpr.set32(1, 1);  // ECX = 1

    u8 code[] = { 0xFF, 0xC9 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(1), 0u);
    EXPECT_TRUE(m_cpu->flags.zf());
    EXPECT_FALSE(m_cpu->flags.sf());
}

TEST_F(IncDecAdcTest, DecZero_Wraps) {
    // FF C9 — DEC ECX with ECX = 0
    m_cpu->gpr.set32(1, 0);

    u8 code[] = { 0xFF, 0xC9 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(1), 0xFFFFFFFFu);
    EXPECT_TRUE(m_cpu->flags.sf());
    EXPECT_FALSE(m_cpu->flags.zf());
}

// ============================================================================
// Interpreter Tests — NEG
// ============================================================================

TEST_F(IncDecAdcTest, NegEcx_Positive) {
    // F7 D9 — NEG ECX
    m_cpu->gpr.set32(1, 5);

    u8 code[] = { 0xF7, 0xD9 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(1), static_cast<u32>(-5));  // 0xFFFFFFFB
    EXPECT_TRUE(m_cpu->flags.cf());   // CF = 1 (operand was non-zero)
    EXPECT_TRUE(m_cpu->flags.sf());
}

TEST_F(IncDecAdcTest, NegZero) {
    // F7 D9 — NEG ECX with ECX = 0
    m_cpu->gpr.set32(1, 0);

    u8 code[] = { 0xF7, 0xD9 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(1), 0u);
    EXPECT_FALSE(m_cpu->flags.cf());  // CF = 0 for zero
    EXPECT_TRUE(m_cpu->flags.zf());
}

TEST_F(IncDecAdcTest, NegMinSigned_Overflow) {
    // F7 D9 — NEG ECX with ECX = 0x80000000
    m_cpu->gpr.set32(1, 0x80000000u);

    u8 code[] = { 0xF7, 0xD9 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(1), 0x80000000u);  // -MIN = MIN for two's complement
    EXPECT_TRUE(m_cpu->flags.of());   // Overflow
    EXPECT_TRUE(m_cpu->flags.cf());   // Non-zero operand
}

// ============================================================================
// Interpreter Tests — ADC
// ============================================================================

TEST_F(IncDecAdcTest, AdcNoCf_BehavesLikeAdd) {
    // 13 C1 — ADC EAX, ECX (r, r/m form)
    m_cpu->gpr.set32(0, 100);  // EAX
    m_cpu->gpr.set32(1, 50);   // ECX
    m_cpu->flags.set_cf(false);

    u8 code[] = { 0x13, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(0), 150u);
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.zf());
}

TEST_F(IncDecAdcTest, AdcWithCf_AddsExtra1) {
    // 13 C1 — ADC EAX, ECX with CF=1
    m_cpu->gpr.set32(0, 100);  // EAX
    m_cpu->gpr.set32(1, 50);   // ECX
    m_cpu->flags.set_cf(true);

    u8 code[] = { 0x13, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(0), 151u);  // 100 + 50 + 1
    EXPECT_FALSE(m_cpu->flags.cf());
}

TEST_F(IncDecAdcTest, AdcAlImm8) {
    // 14 05 — ADC AL, 5
    m_cpu->gpr.set8l(0, 10);  // AL = 10
    m_cpu->flags.set_cf(false);

    u8 code[] = { 0x14, 0x05 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get8l(0), 15);
}

TEST_F(IncDecAdcTest, AdcAlImm8_WithCf) {
    // 14 05 — ADC AL, 5 with CF=1
    m_cpu->gpr.set8l(0, 10);
    m_cpu->flags.set_cf(true);

    u8 code[] = { 0x14, 0x05 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get8l(0), 16);  // 10 + 5 + 1
}

TEST_F(IncDecAdcTest, AdcCfOutputOnOverflow) {
    // 13 C1 — ADC EAX, ECX where result overflows 32 bits
    m_cpu->gpr.set32(0, 0xFFFFFFFFu);
    m_cpu->gpr.set32(1, 0);
    m_cpu->flags.set_cf(true);  // +1 from carry

    u8 code[] = { 0x13, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(0), 0u);
    EXPECT_TRUE(m_cpu->flags.cf());   // Carry out
    EXPECT_TRUE(m_cpu->flags.zf());
}

// ============================================================================
// Interpreter Tests — SBB
// ============================================================================

TEST_F(IncDecAdcTest, SbbNoCf_BehavesLikeSub) {
    // 1B C1 — SBB EAX, ECX
    m_cpu->gpr.set32(0, 100);
    m_cpu->gpr.set32(1, 30);
    m_cpu->flags.set_cf(false);

    u8 code[] = { 0x1B, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(0), 70u);
    EXPECT_FALSE(m_cpu->flags.cf());
}

TEST_F(IncDecAdcTest, SbbWithCf_SubtractsExtra1) {
    // 1B C1 — SBB EAX, ECX with CF=1
    m_cpu->gpr.set32(0, 100);
    m_cpu->gpr.set32(1, 30);
    m_cpu->flags.set_cf(true);

    u8 code[] = { 0x1B, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(0), 69u);  // 100 - 30 - 1
    EXPECT_FALSE(m_cpu->flags.cf());
}

TEST_F(IncDecAdcTest, SbbSetsBorrowCf) {
    // 1B C1 — SBB EAX, ECX where a < b + cf
    m_cpu->gpr.set32(0, 10);
    m_cpu->gpr.set32(1, 10);
    m_cpu->flags.set_cf(true);  // borrow in

    u8 code[] = { 0x1B, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(0), 0xFFFFFFFFu);  // -1
    EXPECT_TRUE(m_cpu->flags.cf());   // Borrow out
    EXPECT_TRUE(m_cpu->flags.sf());
}

// ============================================================================
// Multi-precision ADD + ADC chain
// ============================================================================

TEST_F(IncDecAdcTest, MultiPrecision_AddAdc64Plus64) {
    // Simulate 128-bit add using 64-bit registers:
    // Low:  ADD RAX, RCX   → 48 01 C8
    // High: ADC RDX, RBX   → 48 13 D3

    // First number:  RDX:RAX = 0x00000000_00000001 : 0xFFFFFFFF_FFFFFFFF
    m_cpu->gpr.set64(0, 0xFFFFFFFFFFFFFFFFULL);  // RAX (low)
    m_cpu->gpr.set64(2, 0x0000000000000001ULL);  // RDX (high)

    // Second number: RBX:RCX = 0x00000000_00000000 : 0x00000000_00000002
    m_cpu->gpr.set64(1, 0x0000000000000002ULL);  // RCX (low)
    m_cpu->gpr.set64(3, 0x0000000000000000ULL);  // RBX (high)

    // Step 1: ADD RAX, RCX → 48 01 C8
    u8 add_code[] = { 0x48, 0x01, 0xC8 };
    write_code(add_code, sizeof(add_code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(0), 0x0000000000000001ULL);  // 0xFFFF...FF + 2 = 1 with carry
    EXPECT_TRUE(m_cpu->flags.cf());  // Carry out from low add

    // Step 2: ADC RDX, RBX → 48 13 D3
    m_cpu->rip = 0x1000;  // Reset IP
    u8 adc_code[] = { 0x48, 0x13, 0xD3 };
    write_code(adc_code, sizeof(adc_code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(2), 0x0000000000000002ULL);  // 1 + 0 + carry(1) = 2
    EXPECT_FALSE(m_cpu->flags.cf());

    // Final 128-bit result: RDX:RAX = 0x00000000_00000002 : 0x00000000_00000001
    // Which is correct: 0x1_FFFFFFFFFFFFFFFF + 0x0_0000000000000002 = 0x2_0000000000000001
}

// ============================================================================
// JIT lazy-flag hand-off (threshold > 1 interprets the first visit)
// ============================================================================

namespace {

void stage_pending_cmp32(CpuState& cpu, u32 a, u32 b) {
    cpu.flags.rflags = 0;
    cpu.flags.set_zf(a != b);
    cpu.flags.set_cf(a >= b);
    cpu.flag_op = CpuState::FlagOp::Sub32;
    cpu.flag_src = b;
    cpu.flag_dst = static_cast<u32>(a - b);
}

}  // namespace

TEST_F(IncDecAdcTest, IncRetiresPendingLazyCompare) {
    // JIT CMP 5,5 leaves ZF=1 lazily; poisoned rflags say ZF=0.
    // INC EAX (0→1) must clear ZF and retire the compare so JZ falls through.
    stage_pending_cmp32(*m_cpu, 5, 5);
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RAX), 0);

    u8 inc[] = {0xFF, 0xC0};  // INC EAX
    write_code(inc, sizeof(inc));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(static_cast<u8>(RegIndex::RAX)), 1u);
    EXPECT_EQ(m_cpu->flag_op, CpuState::FlagOp::None);
    EXPECT_FALSE(m_cpu->flags.zf());
    EXPECT_FALSE(m_cpu->flags.cf());

    m_cpu->rip = 0x1000;
    u8 jz[] = {0x74, 0x10};  // JZ +0x10
    write_code(jz, sizeof(jz));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->rip, 0x1002u);
}

TEST_F(IncDecAdcTest, IncPreservesPendingCompareCarry) {
    // CMP 3,5 borrows (CF=1). INC must keep that carry, not the poisoned CF=0.
    stage_pending_cmp32(*m_cpu, 3, 5);
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RAX), 7);

    u8 inc[] = {0xFF, 0xC0};
    write_code(inc, sizeof(inc));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(static_cast<u8>(RegIndex::RAX)), 8u);
    EXPECT_TRUE(m_cpu->flags.cf());
    EXPECT_EQ(m_cpu->flag_op, CpuState::FlagOp::None);
}

TEST_F(IncDecAdcTest, AdcRetiresPendingLazyCompare) {
    // ADC must consume lazy CF and then own flags so a later JZ sees ADC, not CMP.
    stage_pending_cmp32(*m_cpu, 3, 5);  // CF=1
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RAX), 0);
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RCX), 0);

    u8 adc[] = {0x11, 0xC8};  // ADC EAX, ECX  → 0+0+1 = 1, ZF=0
    write_code(adc, sizeof(adc));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(static_cast<u8>(RegIndex::RAX)), 1u);
    EXPECT_EQ(m_cpu->flag_op, CpuState::FlagOp::None);
    EXPECT_FALSE(m_cpu->flags.zf());

    m_cpu->rip = 0x1000;
    u8 jz[] = {0x74, 0x10};
    write_code(jz, sizeof(jz));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->rip, 0x1002u);
}

