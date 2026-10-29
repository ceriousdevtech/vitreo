// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// CALL/RET instruction tests - CPU-013

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
// CALL Decoder Tests
// ============================================================================

TEST(ControlDecoder, CallRel32) {
    // E8 10 00 00 00 - CALL rel32 (+0x10)
    auto inst = decode({0xE8, 0x10, 0x00, 0x00, 0x00});
    EXPECT_EQ(inst.opcode, Opcode::CALL_REL32);
    EXPECT_EQ(inst.length, 5);
    EXPECT_EQ(inst.operand_count, 1);
    EXPECT_EQ(inst.operands[0].type, OperandType::Rel32);
    EXPECT_EQ(inst.operands[0].imm, 0x10);
}

TEST(ControlDecoder, CallRel32Negative) {
    // E8 F0 FF FF FF - CALL rel32 (-16)
    auto inst = decode({0xE8, 0xF0, 0xFF, 0xFF, 0xFF});
    EXPECT_EQ(inst.opcode, Opcode::CALL_REL32);
    EXPECT_EQ(inst.length, 5);
    EXPECT_EQ(inst.operand_count, 1);
    EXPECT_EQ(inst.operands[0].type, OperandType::Rel32);
    EXPECT_EQ(inst.operands[0].imm, -16);
}

// ============================================================================
// RET Decoder Tests
// ============================================================================

TEST(ControlDecoder, RetNear) {
    // C3 - RET
    auto inst = decode({0xC3});
    EXPECT_EQ(inst.opcode, Opcode::RET_NEAR);
    EXPECT_EQ(inst.length, 1);
    EXPECT_EQ(inst.operand_count, 0);
}

TEST(ControlDecoder, RetNearImm16) {
    // C2 08 00 - RET 8
    auto inst = decode({0xC2, 0x08, 0x00});
    EXPECT_EQ(inst.opcode, Opcode::RET_NEAR_IMM16);
    EXPECT_EQ(inst.length, 3);
    EXPECT_EQ(inst.operand_count, 1);
    EXPECT_EQ(inst.operands[0].type, OperandType::Imm16);
    EXPECT_EQ(inst.operands[0].imm, 8);
}

// ============================================================================
// Interpreter Tests
// ============================================================================

class ControlInterpreterTest : public ::testing::Test {
protected:
    static constexpr u64 STACK_TOP = 0x11000;

    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        // Code region
        m_memory->map_region(0x1000, 0x10000, MemoryProtection::ReadWrite);
        // Stack region
        m_memory->map_region(0x10000, 0x1000, MemoryProtection::ReadWrite);

        m_cpu = std::make_unique<CpuState>();
        m_cpu->init_long_mode();
        m_cpu->set_sp(STACK_TOP);
        m_cpu->rip = 0x1000;
    }

    void write_code_at(u64 addr, const u8* code, usize len) {
        m_memory->write_bytes(addr, code, len);
    }

    void write_code(const u8* code, usize len) {
        write_code_at(0x1000, code, len);
    }

    ExecResult execute_one() {
        Interpreter interp(*m_cpu, *m_memory);
        return interp.step();
    }

    u64 read_stack_u64(u64 addr) {
        return m_memory->read_u64(addr).value_or(0);
    }

    std::unique_ptr<GuestMemory> m_memory;
    std::unique_ptr<CpuState> m_cpu;
};

// ============================================================================
// CALL Interpreter Tests
// ============================================================================

TEST_F(ControlInterpreterTest, CallRel32PushesReturnAddr) {
    // E8 10 00 00 00 - CALL rel32 (+0x10)
    // Instruction at 0x1000, length 5 → return addr = 0x1005
    // Target = 0x1005 + 0x10 = 0x1015
    u8 code[] = { 0xE8, 0x10, 0x00, 0x00, 0x00 };
    write_code(code, sizeof(code));

    u64 sp_before = m_cpu->sp();
    auto result = execute_one();

    EXPECT_EQ(result, ExecResult::Branch);
    // SP decremented by 8
    EXPECT_EQ(m_cpu->sp(), sp_before - 8);
    // Return address pushed onto stack
    EXPECT_EQ(read_stack_u64(m_cpu->sp()), 0x1005);
    // RIP jumps to target
    EXPECT_EQ(m_cpu->rip, 0x1015);
}

TEST_F(ControlInterpreterTest, CallRel32Backward) {
    // E8 FB FF FF FF - CALL rel32 (-5)
    // Instruction at 0x1000, length 5 → return addr = 0x1005
    // Target = 0x1005 + (-5) = 0x1000 (loops back)
    u8 code[] = { 0xE8, 0xFB, 0xFF, 0xFF, 0xFF };
    write_code(code, sizeof(code));

    auto result = execute_one();

    EXPECT_EQ(result, ExecResult::Branch);
    EXPECT_EQ(read_stack_u64(m_cpu->sp()), 0x1005);
    EXPECT_EQ(m_cpu->rip, 0x1000);
}

TEST_F(ControlInterpreterTest, CallStackWriteFaultDoesNotCommitRspOrRip) {
    // Intel SDM Vol. 2 CALL: a fault while pushing the return address is
    // precise. The instruction can be restarted with its original RSP/RIP.
    u8 code[] = {0xE8, 0x10, 0x00, 0x00, 0x00};
    write_code(code, sizeof(code));
    constexpr u64 kUnmappedStackTop = 0x20000008ull;
    m_cpu->set_sp(kUnmappedStackTop);

    EXPECT_EQ(execute_one(), ExecResult::Exception);
    EXPECT_EQ(m_cpu->sp(), kUnmappedStackTop);
    EXPECT_EQ(m_cpu->rip, 0x1000u);
}

// ============================================================================
// RET Interpreter Tests
// ============================================================================

TEST_F(ControlInterpreterTest, RetNear) {
    // Set up return address on stack
    u64 return_addr = 0x2000;
    m_cpu->set_sp(STACK_TOP - 8);
    m_memory->write_u64(STACK_TOP - 8, return_addr);

    // C3 - RET
    u8 code[] = { 0xC3 };
    write_code(code, sizeof(code));

    auto result = execute_one();

    EXPECT_EQ(result, ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x2000);
    EXPECT_EQ(m_cpu->sp(), STACK_TOP);  // SP restored (+8)
}

TEST_F(ControlInterpreterTest, RetNearImm16) {
    // Set up return address on stack
    u64 return_addr = 0x2000;
    m_cpu->set_sp(STACK_TOP - 8);
    m_memory->write_u64(STACK_TOP - 8, return_addr);

    // C2 08 00 - RET 8
    u8 code[] = { 0xC2, 0x08, 0x00 };
    write_code(code, sizeof(code));

    auto result = execute_one();

    EXPECT_EQ(result, ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x2000);
    // SP = old_SP + 8 (pop) + 8 (imm16) = STACK_TOP - 8 + 8 + 8
    EXPECT_EQ(m_cpu->sp(), STACK_TOP + 8);
}

// ============================================================================
// CALL + RET Round-Trip
// ============================================================================

TEST_F(ControlInterpreterTest, CallThenRet) {
    // CALL rel32 at 0x1000: E8 0A 00 00 00
    // Target = 0x1005 + 0x0A = 0x100F
    // RET at 0x100F: C3
    u8 call_code[] = { 0xE8, 0x0A, 0x00, 0x00, 0x00 };
    write_code(call_code, sizeof(call_code));

    u8 ret_code[] = { 0xC3 };
    write_code_at(0x100F, ret_code, sizeof(ret_code));

    u64 sp_before = m_cpu->sp();

    // Execute CALL
    auto result = execute_one();
    EXPECT_EQ(result, ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x100F);
    EXPECT_EQ(m_cpu->sp(), sp_before - 8);
    EXPECT_EQ(read_stack_u64(m_cpu->sp()), 0x1005);

    // Execute RET at target
    result = execute_one();
    EXPECT_EQ(result, ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x1005);
    EXPECT_EQ(m_cpu->sp(), sp_before);  // Stack fully restored
}
