// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// PUSH/POP instruction comprehensive tests

#include <gtest/gtest.h>
#include <vector>
#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include "core/cpu/jit/jit_compiler.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

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

// Helper to check decode failure
static void expect_decode_fail(const std::initializer_list<u8>& bytes, DecodeMode mode = DecodeMode::Mode64) {
    std::vector<u8> data(bytes);
    MemoryByteReader reader(data.data(), data.size());
    DecodedInstruction inst;
    DecodeError err = decode_instruction(reader, mode, inst);
    EXPECT_NE(err, DecodeError::Success);
}

// ----------------------------------------------------------------------------
// PUSH r64 (50-57) - 64-bit mode
// ----------------------------------------------------------------------------

TEST(StackDecode, PushRax) {
    // 50 - PUSH RAX
    auto inst = decode({0x50});
    EXPECT_EQ(inst.opcode, Opcode::PUSH_R64);
    EXPECT_EQ(inst.length, 1);
    EXPECT_EQ(inst.operand_count, 1);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 0);  // RAX
}

TEST(StackDecode, PushRcx) {
    // 51 - PUSH RCX
    auto inst = decode({0x51});
    EXPECT_EQ(inst.opcode, Opcode::PUSH_R64);
    EXPECT_EQ(inst.operands[0].reg, 1);  // RCX
}

TEST(StackDecode, PushRbp) {
    // 55 - PUSH RBP
    auto inst = decode({0x55});
    EXPECT_EQ(inst.opcode, Opcode::PUSH_R64);
    EXPECT_EQ(inst.operands[0].reg, 5);  // RBP
}

TEST(StackDecode, PushR8WithRex) {
    // 41 50 - PUSH R8
    auto inst = decode({0x41, 0x50});
    EXPECT_EQ(inst.opcode, Opcode::PUSH_R64);
    EXPECT_EQ(inst.operands[0].reg, 8);  // R8 (REX.B extends)
}

TEST(StackDecode, PushR15WithRex) {
    // 41 57 - PUSH R15
    auto inst = decode({0x41, 0x57});
    EXPECT_EQ(inst.opcode, Opcode::PUSH_R64);
    EXPECT_EQ(inst.operands[0].reg, 15);  // R15
}

// ----------------------------------------------------------------------------
// PUSH imm8 (6A)
// ----------------------------------------------------------------------------

TEST(StackDecode, PushImm8Positive) {
    // 6A 42 - PUSH 0x42
    auto inst = decode({0x6A, 0x42});
    EXPECT_EQ(inst.opcode, Opcode::PUSH_IMM8);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operand_count, 1);
    EXPECT_EQ(inst.operands[0].type, OperandType::Imm8);
    EXPECT_EQ(inst.operands[0].imm, 0x42);
}

TEST(StackDecode, PushImm8Negative) {
    // 6A FF - PUSH -1 (sign-extended)
    auto inst = decode({0x6A, 0xFF});
    EXPECT_EQ(inst.opcode, Opcode::PUSH_IMM8);
    EXPECT_EQ(inst.operands[0].imm, -1);  // Sign-extended
}

// ----------------------------------------------------------------------------
// PUSH imm32 (68)
// ----------------------------------------------------------------------------

TEST(StackDecode, PushImm32) {
    // 68 78 56 34 12 - PUSH 0x12345678
    auto inst = decode({0x68, 0x78, 0x56, 0x34, 0x12});
    EXPECT_EQ(inst.opcode, Opcode::PUSH_IMM32);
    EXPECT_EQ(inst.length, 5);
    EXPECT_EQ(inst.operand_count, 1);
    EXPECT_EQ(inst.operands[0].type, OperandType::Imm32);
    EXPECT_EQ(inst.operands[0].imm, 0x12345678);
}

TEST(StackDecode, PushImm32Negative) {
    // 68 FF FF FF FF - PUSH -1
    auto inst = decode({0x68, 0xFF, 0xFF, 0xFF, 0xFF});
    EXPECT_EQ(inst.opcode, Opcode::PUSH_IMM32);
    EXPECT_EQ(inst.operands[0].imm, -1);
}

// ----------------------------------------------------------------------------
// PUSH r/m64 (FF /6)
// ----------------------------------------------------------------------------

TEST(StackDecode, PushRmRegister) {
    // FF F0 - PUSH RAX (ModRM: C0 = 11 000 000 = reg RAX, but /6 needs F0)
    // Actually FF F0 = PUSH RAX where ModRM = 0xF0 = 11 110 000
    auto inst = decode({0xFF, 0xF0});
    EXPECT_EQ(inst.opcode, Opcode::PUSH_RM64);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operand_count, 1);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 0);  // RAX
}

TEST(StackDecode, PushRmRcx) {
    // FF F1 - PUSH RCX
    auto inst = decode({0xFF, 0xF1});
    EXPECT_EQ(inst.opcode, Opcode::PUSH_RM64);
    EXPECT_EQ(inst.operands[0].reg, 1);  // RCX
}

TEST(StackDecode, PushRmMemoryRax) {
    // FF 30 - PUSH QWORD PTR [RAX]
    auto inst = decode({0xFF, 0x30});
    EXPECT_EQ(inst.opcode, Opcode::PUSH_RM64);
    EXPECT_EQ(inst.operands[0].type, OperandType::Mem64);
    EXPECT_EQ(inst.operands[0].mem.base, 0);  // RAX
    EXPECT_EQ(inst.operands[0].mem.disp, 0);
}

TEST(StackDecode, PushRmMemoryDisp8) {
    // FF 70 10 - PUSH QWORD PTR [RAX+0x10]
    auto inst = decode({0xFF, 0x70, 0x10});
    EXPECT_EQ(inst.opcode, Opcode::PUSH_RM64);
    EXPECT_EQ(inst.operands[0].type, OperandType::Mem64);
    EXPECT_EQ(inst.operands[0].mem.base, 0);  // RAX
    EXPECT_EQ(inst.operands[0].mem.disp, 0x10);
}

TEST(StackDecode, PushRmMemoryRipRelative) {
    // FF 35 00 10 00 00 - PUSH QWORD PTR [RIP+0x1000]
    auto inst = decode({0xFF, 0x35, 0x00, 0x10, 0x00, 0x00});
    EXPECT_EQ(inst.opcode, Opcode::PUSH_RM64);
    EXPECT_EQ(inst.operands[0].type, OperandType::Mem64);
    EXPECT_TRUE(inst.operands[0].mem.is_rip_relative());
    EXPECT_EQ(inst.operands[0].mem.disp, 0x1000);
}

// ----------------------------------------------------------------------------
// POP r64 (58-5F) - 64-bit mode
// ----------------------------------------------------------------------------

TEST(StackDecode, PopRax) {
    // 58 - POP RAX
    auto inst = decode({0x58});
    EXPECT_EQ(inst.opcode, Opcode::POP_R64);
    EXPECT_EQ(inst.length, 1);
    EXPECT_EQ(inst.operand_count, 1);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 0);  // RAX
}

TEST(StackDecode, PopRcx) {
    // 59 - POP RCX
    auto inst = decode({0x59});
    EXPECT_EQ(inst.opcode, Opcode::POP_R64);
    EXPECT_EQ(inst.operands[0].reg, 1);  // RCX
}

TEST(StackDecode, PopRbp) {
    // 5D - POP RBP
    auto inst = decode({0x5D});
    EXPECT_EQ(inst.opcode, Opcode::POP_R64);
    EXPECT_EQ(inst.operands[0].reg, 5);  // RBP
}

TEST(StackDecode, PopR8WithRex) {
    // 41 58 - POP R8
    auto inst = decode({0x41, 0x58});
    EXPECT_EQ(inst.opcode, Opcode::POP_R64);
    EXPECT_EQ(inst.operands[0].reg, 8);  // R8
}

TEST(StackDecode, PopR15WithRex) {
    // 41 5F - POP R15
    auto inst = decode({0x41, 0x5F});
    EXPECT_EQ(inst.opcode, Opcode::POP_R64);
    EXPECT_EQ(inst.operands[0].reg, 15);  // R15
}

// ----------------------------------------------------------------------------
// POP r/m64 (8F /0)
// ----------------------------------------------------------------------------

TEST(StackDecode, PopRmRegister) {
    // 8F C0 - POP RAX (ModRM: C0 = 11 000 000 = reg RAX)
    auto inst = decode({0x8F, 0xC0});
    EXPECT_EQ(inst.opcode, Opcode::POP_RM64);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operand_count, 1);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 0);  // RAX
}

TEST(StackDecode, PopRmRcx) {
    // 8F C1 - POP RCX
    auto inst = decode({0x8F, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::POP_RM64);
    EXPECT_EQ(inst.operands[0].reg, 1);  // RCX
}

TEST(StackDecode, PopRmMemoryRax) {
    // 8F 00 - POP QWORD PTR [RAX]
    auto inst = decode({0x8F, 0x00});
    EXPECT_EQ(inst.opcode, Opcode::POP_RM64);
    EXPECT_EQ(inst.operands[0].type, OperandType::Mem64);
    EXPECT_EQ(inst.operands[0].mem.base, 0);  // RAX
}

TEST(StackDecode, PopRmMemoryDisp32) {
    // 8F 80 00 10 00 00 - POP QWORD PTR [RAX+0x1000]
    auto inst = decode({0x8F, 0x80, 0x00, 0x10, 0x00, 0x00});
    EXPECT_EQ(inst.opcode, Opcode::POP_RM64);
    EXPECT_EQ(inst.operands[0].type, OperandType::Mem64);
    EXPECT_EQ(inst.operands[0].mem.disp, 0x1000);
}

TEST(StackDecode, PopRmInvalidRegField) {
    // 8F C8 - Invalid (reg field = 1, only /0 is valid)
    expect_decode_fail({0x8F, 0xC8});
}

// ----------------------------------------------------------------------------
// 32-bit mode tests
// ----------------------------------------------------------------------------

TEST(StackDecode, PushEax32BitMode) {
    // 50 - PUSH EAX (32-bit mode)
    auto inst = decode({0x50}, DecodeMode::Mode32);
    EXPECT_EQ(inst.opcode, Opcode::PUSH_R32);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 0);  // EAX
}

TEST(StackDecode, PopEax32BitMode) {
    // 58 - POP EAX (32-bit mode)
    auto inst = decode({0x58}, DecodeMode::Mode32);
    EXPECT_EQ(inst.opcode, Opcode::POP_R32);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 0);  // EAX
}

TEST(StackDecode, PushfwOperandSizePrefix) {
    // 66 9C — PUSHF with 16-bit operand size (Petite and other PE32 packers).
    auto inst = decode({0x66, 0x9C}, DecodeMode::Mode32);
    EXPECT_EQ(inst.opcode, Opcode::PUSHF);
    EXPECT_TRUE(inst.prefixes.operand_size);
    EXPECT_EQ(inst.length, 2);
}

TEST(StackDecode, PopfwOperandSizePrefix) {
    auto inst = decode({0x66, 0x9D}, DecodeMode::Mode32);
    EXPECT_EQ(inst.opcode, Opcode::POPF);
    EXPECT_TRUE(inst.prefixes.operand_size);
    EXPECT_EQ(inst.length, 2);
}

// ----------------------------------------------------------------------------
// PUSH/POP FS/GS (0F A0/A1/A8/A9) — valid in 32-bit and 64-bit
// ----------------------------------------------------------------------------

TEST(StackDecode, PushFs32) {
    auto inst = decode({0x0F, 0xA0}, DecodeMode::Mode32);
    EXPECT_EQ(inst.opcode, Opcode::PUSH_FS);
    EXPECT_EQ(inst.operand_count, 1);
    EXPECT_EQ(inst.operands[0].type, OperandType::RegSeg);
    EXPECT_EQ(inst.operands[0].reg, static_cast<u8>(SegReg::FS));
}

TEST(StackDecode, PushFs64) {
    auto inst = decode({0x0F, 0xA0}, DecodeMode::Mode64);
    EXPECT_EQ(inst.opcode, Opcode::PUSH_FS);
    EXPECT_EQ(inst.operand_count, 1);
    EXPECT_EQ(inst.operands[0].type, OperandType::RegSeg);
    EXPECT_EQ(inst.operands[0].reg, static_cast<u8>(SegReg::FS));
}

TEST(StackDecode, PushGs32) {
    auto inst = decode({0x0F, 0xA8}, DecodeMode::Mode32);
    EXPECT_EQ(inst.opcode, Opcode::PUSH_GS);
    EXPECT_EQ(inst.operands[0].type, OperandType::RegSeg);
    EXPECT_EQ(inst.operands[0].reg, static_cast<u8>(SegReg::GS));
}

TEST(StackDecode, PushGs64) {
    auto inst = decode({0x0F, 0xA8}, DecodeMode::Mode64);
    EXPECT_EQ(inst.opcode, Opcode::PUSH_GS);
    EXPECT_EQ(inst.operands[0].type, OperandType::RegSeg);
    EXPECT_EQ(inst.operands[0].reg, static_cast<u8>(SegReg::GS));
}

TEST(StackDecode, PopFs32) {
    auto inst = decode({0x0F, 0xA1}, DecodeMode::Mode32);
    EXPECT_EQ(inst.opcode, Opcode::POP_FS);
    EXPECT_EQ(inst.operands[0].type, OperandType::RegSeg);
    EXPECT_EQ(inst.operands[0].reg, static_cast<u8>(SegReg::FS));
}

TEST(StackDecode, PopFs64) {
    auto inst = decode({0x0F, 0xA1}, DecodeMode::Mode64);
    EXPECT_EQ(inst.opcode, Opcode::POP_FS);
    EXPECT_EQ(inst.operands[0].type, OperandType::RegSeg);
    EXPECT_EQ(inst.operands[0].reg, static_cast<u8>(SegReg::FS));
}

TEST(StackDecode, PopGs32) {
    auto inst = decode({0x0F, 0xA9}, DecodeMode::Mode32);
    EXPECT_EQ(inst.opcode, Opcode::POP_GS);
    EXPECT_EQ(inst.operands[0].type, OperandType::RegSeg);
    EXPECT_EQ(inst.operands[0].reg, static_cast<u8>(SegReg::GS));
}

TEST(StackDecode, PopGs64) {
    auto inst = decode({0x0F, 0xA9}, DecodeMode::Mode64);
    EXPECT_EQ(inst.opcode, Opcode::POP_GS);
    EXPECT_EQ(inst.operands[0].type, OperandType::RegSeg);
    EXPECT_EQ(inst.operands[0].reg, static_cast<u8>(SegReg::GS));
}

// ============================================================================
// Interpreter Tests
// ============================================================================

class StackInterpTest : public ::testing::Test {
protected:
    static constexpr u64 STACK_BASE = 0x10000;
    static constexpr u64 STACK_SIZE = 0x1000;
    static constexpr u64 CODE_BASE = 0x1000;
    
    GuestMemory memory;
    CpuState cpu;
    
    void SetUp() override {
        // Map stack memory
        memory.map_region(STACK_BASE, STACK_SIZE, 
            MemoryProtection::ReadWrite);
        // Map code memory (ReadWrite suffices: write_u8 needs Write,
        // interpreter read_u8 only checks mapping not protection)
        memory.map_region(CODE_BASE, 0x1000,
            MemoryProtection::ReadWrite);
        
        // Initialize CPU state
        cpu.init_long_mode();
        cpu.set_sp(STACK_BASE + STACK_SIZE);  // Stack grows down
        cpu.rip = CODE_BASE;
    }
    
    ExecResult execute(const std::initializer_list<u8>& code) {
        // Write code to memory
        u64 addr = CODE_BASE;
        for (u8 byte : code) {
            memory.write_u8(addr++, byte);
        }
        
        Interpreter interp(cpu, memory);
        return interp.step();
    }
    
    u64 read_stack_u64(u64 offset = 0) {
        return memory.read_u64(cpu.sp() + offset).value_or(0);
    }
    
    void write_stack_u64(u64 value, u64 offset = 0) {
        memory.write_u64(cpu.sp() + offset, value);
    }
};

// ----------------------------------------------------------------------------
// PUSH execution tests
// ----------------------------------------------------------------------------

TEST_F(StackInterpTest, PushRaxDecrementsRsp) {
    u64 initial_sp = cpu.sp();
    cpu.gpr.set64(0, 0x1234567890ABCDEF);  // RAX
    
    // 50 - PUSH RAX
    auto result = execute({0x50});
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(cpu.sp(), initial_sp - 8);
    EXPECT_EQ(read_stack_u64(), 0x1234567890ABCDEF);
}

TEST_F(StackInterpTest, PushRbp) {
    u64 initial_sp = cpu.sp();
    cpu.gpr.set64(5, 0xDEADBEEFCAFEBABE);  // RBP
    
    // 55 - PUSH RBP
    auto result = execute({0x55});
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(cpu.sp(), initial_sp - 8);
    EXPECT_EQ(read_stack_u64(), 0xDEADBEEFCAFEBABE);
}

TEST_F(StackInterpTest, PushImm8SignExtended) {
    u64 initial_sp = cpu.sp();
    
    // 6A FF - PUSH -1
    auto result = execute({0x6A, 0xFF});
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(cpu.sp(), initial_sp - 8);
    EXPECT_EQ(read_stack_u64(), 0xFFFFFFFFFFFFFFFF);  // Sign-extended to 64-bit
}

TEST_F(StackInterpTest, PushImm8Positive) {
    u64 initial_sp = cpu.sp();
    
    // 6A 42 - PUSH 0x42
    auto result = execute({0x6A, 0x42});
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(cpu.sp(), initial_sp - 8);
    EXPECT_EQ(read_stack_u64(), 0x42);
}

TEST_F(StackInterpTest, PushImm32) {
    u64 initial_sp = cpu.sp();
    
    // 68 78 56 34 12 - PUSH 0x12345678
    auto result = execute({0x68, 0x78, 0x56, 0x34, 0x12});
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(cpu.sp(), initial_sp - 8);
    EXPECT_EQ(read_stack_u64(), 0x12345678);
}

TEST_F(StackInterpTest, PushImm32SignExtended) {
    u64 initial_sp = cpu.sp();
    
    // 68 FF FF FF FF - PUSH -1
    auto result = execute({0x68, 0xFF, 0xFF, 0xFF, 0xFF});
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(cpu.sp(), initial_sp - 8);
    EXPECT_EQ(read_stack_u64(), 0xFFFFFFFFFFFFFFFF);  // Sign-extended
}

TEST_F(StackInterpTest, PushR8Extended) {
    u64 initial_sp = cpu.sp();
    cpu.gpr.set64(8, 0xABCDEF0123456789);  // R8
    
    // 41 50 - PUSH R8
    auto result = execute({0x41, 0x50});
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(cpu.sp(), initial_sp - 8);
    EXPECT_EQ(read_stack_u64(), 0xABCDEF0123456789);
}

TEST_F(StackInterpTest, PushRmRegister) {
    u64 initial_sp = cpu.sp();
    cpu.gpr.set64(0, 0x1111222233334444);  // RAX
    
    // FF F0 - PUSH RAX (via r/m encoding)
    auto result = execute({0xFF, 0xF0});
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(cpu.sp(), initial_sp - 8);
    EXPECT_EQ(read_stack_u64(), 0x1111222233334444);
}

TEST_F(StackInterpTest, PushRmMemory) {
    u64 initial_sp = cpu.sp();
    
    // Store value in memory (use some safe location)
    u64 mem_addr = STACK_BASE + 0x100;
    memory.write_u64(mem_addr, 0x5555666677778888);
    cpu.gpr.set64(0, mem_addr);  // RAX = address to read from
    
    // FF 30 - PUSH QWORD PTR [RAX]
    auto result = execute({0xFF, 0x30});
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(cpu.sp(), initial_sp - 8);
    EXPECT_EQ(read_stack_u64(), 0x5555666677778888);
}

// ----------------------------------------------------------------------------
// POP execution tests
// ----------------------------------------------------------------------------

TEST_F(StackInterpTest, PopRaxIncrementsRsp) {
    // Push a value first
    u64 initial_sp = cpu.sp() - 8;
    cpu.set_sp(initial_sp);
    write_stack_u64(0x1234567890ABCDEF);
    
    // 58 - POP RAX
    auto result = execute({0x58});
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(cpu.sp(), initial_sp + 8);
    EXPECT_EQ(cpu.gpr.get64(0), 0x1234567890ABCDEF);
}

TEST_F(StackInterpTest, PopRbp) {
    u64 initial_sp = cpu.sp() - 8;
    cpu.set_sp(initial_sp);
    write_stack_u64(0xDEADBEEFCAFEBABE);
    
    // 5D - POP RBP
    auto result = execute({0x5D});
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(cpu.sp(), initial_sp + 8);
    EXPECT_EQ(cpu.gpr.get64(5), 0xDEADBEEFCAFEBABE);
}

TEST_F(StackInterpTest, PopR8Extended) {
    u64 initial_sp = cpu.sp() - 8;
    cpu.set_sp(initial_sp);
    write_stack_u64(0xABCDEF0123456789);
    
    // 41 58 - POP R8
    auto result = execute({0x41, 0x58});
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(cpu.sp(), initial_sp + 8);
    EXPECT_EQ(cpu.gpr.get64(8), 0xABCDEF0123456789);
}

TEST_F(StackInterpTest, PopRmRegister) {
    u64 initial_sp = cpu.sp() - 8;
    cpu.set_sp(initial_sp);
    write_stack_u64(0x9999AAAABBBBCCCCULL);
    
    // 8F C0 - POP RAX (via r/m encoding)
    auto result = execute({0x8F, 0xC0});
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(cpu.sp(), initial_sp + 8);
    EXPECT_EQ(cpu.gpr.get64(0), 0x9999AAAABBBBCCCCULL);
}

TEST_F(StackInterpTest, PopRmMemory) {
    u64 initial_sp = cpu.sp() - 8;
    cpu.set_sp(initial_sp);
    write_stack_u64(0x5555666677778888);
    
    // Destination address
    u64 mem_addr = STACK_BASE + 0x100;
    cpu.gpr.set64(1, mem_addr);  // RCX = destination address
    
    // 8F 01 - POP QWORD PTR [RCX]
    auto result = execute({0x8F, 0x01});
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(cpu.sp(), initial_sp + 8);
    EXPECT_EQ(memory.read_u64(mem_addr).value_or(0), 0x5555666677778888);
}

// ----------------------------------------------------------------------------
// PUSH/POP round-trip tests
// ----------------------------------------------------------------------------

TEST_F(StackInterpTest, PushPopRoundTrip) {
    u64 initial_sp = cpu.sp();
    cpu.gpr.set64(0, 0x1234567890ABCDEF);  // RAX
    cpu.gpr.set64(1, 0);  // RCX = 0
    
    // PUSH RAX
    execute({0x50});
    EXPECT_EQ(cpu.sp(), initial_sp - 8);
    
    // POP RCX
    cpu.rip = CODE_BASE;  // Reset IP
    execute({0x59});
    EXPECT_EQ(cpu.sp(), initial_sp);
    EXPECT_EQ(cpu.gpr.get64(1), 0x1234567890ABCDEF);  // RCX = original RAX value
}

TEST_F(StackInterpTest, MultiplePushes) {
    u64 initial_sp = cpu.sp();
    cpu.gpr.set64(0, 0x1111111111111111);  // RAX
    cpu.gpr.set64(1, 0x2222222222222222);  // RCX
    cpu.gpr.set64(2, 0x3333333333333333);  // RDX
    
    // PUSH RAX, PUSH RCX, PUSH RDX
    execute({0x50});
    cpu.rip = CODE_BASE;
    execute({0x51});
    cpu.rip = CODE_BASE;
    execute({0x52});
    
    EXPECT_EQ(cpu.sp(), initial_sp - 24);
    EXPECT_EQ(read_stack_u64(0), 0x3333333333333333);   // RDX (top)
    EXPECT_EQ(read_stack_u64(8), 0x2222222222222222);   // RCX
    EXPECT_EQ(read_stack_u64(16), 0x1111111111111111);  // RAX (bottom)
}

TEST_F(StackInterpTest, PushFsDecrementsRspBy8) {
    // Intel SDM Vol. 2 PUSH: 64-bit default operand size stores a
    // zero-extended selector and decrements RSP by 8.
    const u64 initial_sp = cpu.sp();
    cpu.seg.fs = 0x0053;
    auto result = execute({0x0F, 0xA0});  // PUSH FS
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(cpu.sp(), initial_sp - 8);
    EXPECT_EQ(memory.read_u16(cpu.sp()).value_or(0), 0x0053u);
    EXPECT_EQ(read_stack_u64(), 0x0053u);
}

TEST_F(StackInterpTest, PushGsPopFsRoundTrip) {
    cpu.seg.gs = 0x002B;
    cpu.seg.fs = 0;
    auto result = execute({0x0F, 0xA8});  // PUSH GS
    EXPECT_EQ(result, ExecResult::Continue);
    cpu.rip = CODE_BASE;
    result = execute({0x0F, 0xA1});  // POP FS
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(cpu.seg.fs, 0x002Bu);
}

TEST_F(StackInterpTest, MultiplePops) {
    // Set up stack with values
    cpu.set_sp(cpu.sp() - 24);
    write_stack_u64(0x3333333333333333, 0);   // Top
    write_stack_u64(0x2222222222222222, 8);   // Middle
    write_stack_u64(0x1111111111111111, 16);  // Bottom
    
    u64 initial_sp = cpu.sp();
    
    // POP RDX, POP RCX, POP RAX
    execute({0x5A});  // POP RDX
    cpu.rip = CODE_BASE;
    execute({0x59});  // POP RCX
    cpu.rip = CODE_BASE;
    execute({0x58});  // POP RAX
    
    EXPECT_EQ(cpu.sp(), initial_sp + 24);
    EXPECT_EQ(cpu.gpr.get64(2), 0x3333333333333333);  // RDX
    EXPECT_EQ(cpu.gpr.get64(1), 0x2222222222222222);  // RCX
    EXPECT_EQ(cpu.gpr.get64(0), 0x1111111111111111);  // RAX
}

// ----------------------------------------------------------------------------
// 32-bit PUSHF/POPF operand-size (Intel SDM Vol. 2 PUSHF/POPF)
// ----------------------------------------------------------------------------

class StackInterp32Test : public ::testing::Test {
protected:
    static constexpr u64 STACK_BASE = 0x10000;
    static constexpr u64 STACK_SIZE = 0x1000;
    static constexpr u64 CODE_BASE = 0x1000;

    GuestMemory memory;
    CpuState cpu;

    void SetUp() override {
        memory.map_region(STACK_BASE, STACK_SIZE, MemoryProtection::ReadWrite);
        memory.map_region(CODE_BASE, 0x1000, MemoryProtection::ReadWrite);
        cpu.init_protected_mode();
        cpu.set_sp(STACK_BASE + STACK_SIZE);
        cpu.rip = CODE_BASE;
    }

    ExecResult execute(const std::initializer_list<u8>& code) {
        u64 addr = CODE_BASE;
        for (u8 byte : code) {
            memory.write_u8(addr++, byte);
        }
        Interpreter interp(cpu, memory);
        return interp.step();
    }
};

TEST_F(StackInterp32Test, PushfdDecrementsEspBy4) {
    const u32 initial_sp = static_cast<u32>(cpu.sp());
    cpu.flags.rflags = 0x246;  // ZF|PF|bit1
    auto result = execute({0x9C});  // PUSHFD
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(static_cast<u32>(cpu.sp()), initial_sp - 4u);
    EXPECT_EQ(memory.read_u32(cpu.sp()).value_or(0), 0x246u);
}

TEST_F(StackInterp32Test, PushfwDecrementsEspBy2) {
    // Petite installs SEH then executes `66 9C` before PUSHAD. Pushing 4 bytes
    // instead of 2 shifts the saved frame so a later `mov [ecx+0x2e], esp`
    // overwrites the SEH handler pointer.
    const u32 initial_sp = static_cast<u32>(cpu.sp());
    cpu.flags.rflags = 0x846;  // SF|ZF|PF|bit1
    auto result = execute({0x66, 0x9C});  // PUSHFW
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(static_cast<u32>(cpu.sp()), initial_sp - 2u);
    EXPECT_EQ(memory.read_u16(cpu.sp()).value_or(0), 0x846u);
}

TEST_F(StackInterp32Test, PopfwConsumes2Bytes) {
    const u32 initial_sp = static_cast<u32>(cpu.sp());
    cpu.set_sp(initial_sp - 2);
    ASSERT_TRUE(memory.write_u16(cpu.sp(), 0x0202).is_ok());  // IF|bit1
    cpu.flags.rflags = 0;
    auto result = execute({0x66, 0x9D});  // POPFW
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(static_cast<u32>(cpu.sp()), initial_sp);
    EXPECT_EQ(cpu.flags.rflags & 0xFFFFu, 0x0202u);
}

TEST_F(StackInterp32Test, PushFsPopGsRoundTrip) {
    const u32 initial_sp = static_cast<u32>(cpu.sp());
    cpu.seg.fs = 0x0023;
    cpu.seg.gs = 0;
    auto result = execute({0x0F, 0xA0});  // PUSH FS
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(static_cast<u32>(cpu.sp()), initial_sp - 4u);
    EXPECT_EQ(memory.read_u16(cpu.sp()).value_or(0), 0x0023u);

    cpu.rip = CODE_BASE;
    result = execute({0x0F, 0xA9});  // POP GS
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(static_cast<u32>(cpu.sp()), initial_sp);
    EXPECT_EQ(cpu.seg.gs, 0x0023u);
}

// ----------------------------------------------------------------------------
// IRETD / RETF — user-mode same-privilege (Intel SDM Vol. 2)
// ----------------------------------------------------------------------------

TEST_F(StackInterp32Test, IretdSamePrivilege) {
    const u32 initial_sp = static_cast<u32>(cpu.sp());
    const u32 new_eip = 0x2000;
    const u16 new_cs = 0x001B;
    const u32 new_eflags = 0x00000246;  // ZF|PF|bit1|IF
    cpu.set_sp(initial_sp - 12);
    ASSERT_TRUE(memory.write_u32(cpu.sp(), new_eip).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, new_cs).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 8, new_eflags).is_ok());
    cpu.flags.rflags = 0;
    cpu.seg.cs = 0x0008;

    auto result = execute({0xCF});  // IRETD
    EXPECT_EQ(result, ExecResult::Branch);
    EXPECT_EQ(cpu.rip, new_eip);
    EXPECT_EQ(cpu.seg.cs, new_cs);
    EXPECT_EQ(static_cast<u32>(cpu.sp()), initial_sp);
    EXPECT_EQ(cpu.flags.rflags & 0x00244FD7u, new_eflags);
}

TEST_F(StackInterp32Test, Retf) {
    const u32 initial_sp = static_cast<u32>(cpu.sp());
    const u32 new_eip = 0x1800;
    const u16 new_cs = 0x0023;
    cpu.set_sp(initial_sp - 8);
    ASSERT_TRUE(memory.write_u32(cpu.sp(), new_eip).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, new_cs).is_ok());
    cpu.seg.cs = 0x0008;

    auto result = execute({0xCB});  // RETF
    EXPECT_EQ(result, ExecResult::Branch);
    EXPECT_EQ(cpu.rip, new_eip);
    EXPECT_EQ(cpu.seg.cs, new_cs);
    EXPECT_EQ(static_cast<u32>(cpu.sp()), initial_sp);
}

TEST_F(StackInterp32Test, RetfImm16) {
    const u32 initial_sp = static_cast<u32>(cpu.sp());
    const u32 new_eip = 0x1800;
    const u16 new_cs = 0x0023;
    cpu.set_sp(initial_sp - 8);
    ASSERT_TRUE(memory.write_u32(cpu.sp(), new_eip).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, new_cs).is_ok());

    auto result = execute({0xCA, 0x08, 0x00});  // RETF 8
    EXPECT_EQ(result, ExecResult::Branch);
    EXPECT_EQ(cpu.rip, new_eip);
    EXPECT_EQ(cpu.seg.cs, new_cs);
    EXPECT_EQ(static_cast<u32>(cpu.sp()), initial_sp + 8u);
}

TEST_F(StackInterpTest, IretqSamePrivilege) {
    const u64 initial_sp = cpu.sp();
    const u64 new_rip = 0x2000;
    const u16 new_cs = 0x0033;
    const u64 new_rflags = 0x0000000000000246ull;
    const u64 new_rsp = STACK_BASE + 0x800;
    const u16 new_ss = 0x002B;
    cpu.set_sp(initial_sp - 40);
    ASSERT_TRUE(memory.write_u64(cpu.sp(), new_rip).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.sp() + 8, new_cs).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.sp() + 16, new_rflags).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.sp() + 24, new_rsp).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.sp() + 32, new_ss).is_ok());
    cpu.flags.rflags = 0;
    cpu.seg.cs = 0x0008;
    cpu.seg.ss = 0x0010;

    auto result = execute({0xCF});  // IRETQ
    EXPECT_EQ(result, ExecResult::Branch);
    EXPECT_EQ(cpu.rip, new_rip);
    EXPECT_EQ(cpu.seg.cs, new_cs);
    EXPECT_EQ(cpu.sp(), new_rsp);
    EXPECT_EQ(cpu.seg.ss, new_ss);
    EXPECT_EQ(cpu.flags.rflags & 0x00244FD7ull, new_rflags);
}

TEST_F(StackInterpTest, Retf) {
    const u64 initial_sp = cpu.sp();
    const u64 new_rip = 0x1800;
    const u16 new_cs = 0x0033;
    cpu.set_sp(initial_sp - 16);
    ASSERT_TRUE(memory.write_u64(cpu.sp(), new_rip).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.sp() + 8, new_cs).is_ok());
    cpu.seg.cs = 0x0008;

    auto result = execute({0xCB});  // RETF
    EXPECT_EQ(result, ExecResult::Branch);
    EXPECT_EQ(cpu.rip, new_rip);
    EXPECT_EQ(cpu.seg.cs, new_cs);
    EXPECT_EQ(cpu.sp(), initial_sp);
}

TEST_F(StackInterp32Test, FarJmpPtr16_32) {
    const u32 initial_sp = static_cast<u32>(cpu.sp());
    auto result = execute({0xEA, 0x78, 0x56, 0x34, 0x12, 0x00, 0x08});
    EXPECT_EQ(result, ExecResult::Branch);
    EXPECT_EQ(cpu.rip, 0x12345678u);
    EXPECT_EQ(cpu.seg.cs, 0x0800u);
    EXPECT_EQ(static_cast<u32>(cpu.sp()), initial_sp);
}

TEST_F(StackInterp32Test, FarCallPtr16_32ThenRetf) {
    const u32 initial_sp = static_cast<u32>(cpu.sp());
    const u16 start_cs = cpu.seg.cs;
    ASSERT_TRUE(memory.write_u8(0x1800, 0xCB).is_ok());  // RETF at target

    auto result = execute({0x9A, 0x00, 0x18, 0x00, 0x00, 0x23, 0x00});
    EXPECT_EQ(result, ExecResult::Branch);
    EXPECT_EQ(cpu.rip, 0x1800u);
    EXPECT_EQ(cpu.seg.cs, 0x0023u);
    EXPECT_EQ(static_cast<u32>(cpu.sp()), initial_sp - 8u);
    EXPECT_EQ(memory.read_u32(cpu.sp()).value_or(0), CODE_BASE + 7u);
    EXPECT_EQ(memory.read_u32(cpu.sp() + 4).value_or(0), start_cs);

    Interpreter interp(cpu, memory);
    result = interp.step();
    EXPECT_EQ(result, ExecResult::Branch);
    EXPECT_EQ(cpu.rip, CODE_BASE + 7u);
    EXPECT_EQ(cpu.seg.cs, start_cs);
    EXPECT_EQ(static_cast<u32>(cpu.sp()), initial_sp);
}

TEST_F(StackInterp32Test, FarJmpM16_32) {
    const u32 initial_sp = static_cast<u32>(cpu.sp());
    const u64 ptr = STACK_BASE;
    ASSERT_TRUE(memory.write_u32(ptr, 0x2000).is_ok());
    ASSERT_TRUE(memory.write_u16(ptr + 4, 0x001B).is_ok());

    auto result = execute({0xFF, 0x2D, 0x00, 0x00, 0x01, 0x00});  // JMP far [0x10000]
    EXPECT_EQ(result, ExecResult::Branch);
    EXPECT_EQ(cpu.rip, 0x2000u);
    EXPECT_EQ(cpu.seg.cs, 0x001Bu);
    EXPECT_EQ(static_cast<u32>(cpu.sp()), initial_sp);
}

TEST_F(StackInterpTest, FarJmpM16_64) {
    const u64 initial_sp = cpu.sp();
    cpu.gpr.rax = STACK_BASE;
    ASSERT_TRUE(memory.write_u64(STACK_BASE, 0x1800).is_ok());
    ASSERT_TRUE(memory.write_u16(STACK_BASE + 8, 0x0033).is_ok());

    auto result = execute({0xFF, 0x28});  // JMP far [RAX]
    EXPECT_EQ(result, ExecResult::Branch);
    EXPECT_EQ(cpu.rip, 0x1800u);
    EXPECT_EQ(cpu.seg.cs, 0x0033u);
    EXPECT_EQ(cpu.sp(), initial_sp);
}

TEST_F(StackInterpTest, FarJmpM16_16) {
    const u64 initial_sp = cpu.sp();
    cpu.gpr.rax = STACK_BASE;
    ASSERT_TRUE(memory.write_u16(STACK_BASE, 0x1800).is_ok());
    ASSERT_TRUE(memory.write_u16(STACK_BASE + 2, 0x0033).is_ok());

    auto result = execute({0x66, 0xFF, 0x28});  // 66H JMP far [RAX]  FF /5
    EXPECT_EQ(result, ExecResult::Branch);
    EXPECT_EQ(cpu.rip, 0x1800u);
    EXPECT_EQ(cpu.seg.cs, 0x0033u);
    EXPECT_EQ(cpu.sp(), initial_sp);
}

TEST_F(StackInterpTest, FarCallM16_16ThenRetf) {
    const u64 initial_sp = cpu.sp();
    const u16 start_cs = cpu.seg.cs;
    cpu.gpr.rax = STACK_BASE;
    ASSERT_TRUE(memory.write_u16(STACK_BASE, 0x1800).is_ok());
    ASSERT_TRUE(memory.write_u16(STACK_BASE + 2, 0x0033).is_ok());
    ASSERT_TRUE(memory.write_u8(0x1800, 0xCB).is_ok());

    auto result = execute({0x66, 0xFF, 0x18});  // 66H CALL far [RAX]  FF /3
    EXPECT_EQ(result, ExecResult::Branch);
    EXPECT_EQ(cpu.rip, 0x1800u);
    EXPECT_EQ(cpu.seg.cs, 0x0033u);
    EXPECT_EQ(cpu.sp(), initial_sp - 16u);
    EXPECT_EQ(memory.read_u64(cpu.sp()).value_or(0), CODE_BASE + 3u);
    EXPECT_EQ(memory.read_u64(cpu.sp() + 8).value_or(0), start_cs);

    Interpreter interp(cpu, memory);
    result = interp.step();
    EXPECT_EQ(result, ExecResult::Branch);
    EXPECT_EQ(cpu.rip, CODE_BASE + 3u);
    EXPECT_EQ(cpu.seg.cs, start_cs);
    EXPECT_EQ(cpu.sp(), initial_sp);
}

TEST_F(StackInterpTest, FarCallM16_64ThenRetf) {
    const u64 initial_sp = cpu.sp();
    const u16 start_cs = cpu.seg.cs;
    cpu.gpr.rax = STACK_BASE;
    ASSERT_TRUE(memory.write_u64(STACK_BASE, 0x1800).is_ok());
    ASSERT_TRUE(memory.write_u16(STACK_BASE + 8, 0x0033).is_ok());
    ASSERT_TRUE(memory.write_u8(0x1800, 0xCB).is_ok());

    auto result = execute({0xFF, 0x18});  // CALL far [RAX]
    EXPECT_EQ(result, ExecResult::Branch);
    EXPECT_EQ(cpu.rip, 0x1800u);
    EXPECT_EQ(cpu.seg.cs, 0x0033u);
    EXPECT_EQ(cpu.sp(), initial_sp - 16u);
    EXPECT_EQ(memory.read_u64(cpu.sp()).value_or(0), CODE_BASE + 2u);
    EXPECT_EQ(memory.read_u64(cpu.sp() + 8).value_or(0), start_cs);

    Interpreter interp(cpu, memory);
    result = interp.step();
    EXPECT_EQ(result, ExecResult::Branch);
    EXPECT_EQ(cpu.rip, CODE_BASE + 2u);
    EXPECT_EQ(cpu.seg.cs, start_cs);
    EXPECT_EQ(cpu.sp(), initial_sp);
}

namespace {

BasicBlock decode_far_block(const u8* code, usize len, DecodeMode mode, u64 start) {
    BasicBlock block;
    block.start_addr = start;
    block.is_64bit = (mode == DecodeMode::Mode64);
    MemoryByteReader reader(code, len);
    while (reader.remaining() > 0) {
        DecodedInstruction inst;
        if (decode_instruction(reader, mode, inst) != DecodeError::Success) {
            break;
        }
        block.instructions.push_back(inst);
    }
    block.byte_length = static_cast<u32>(reader.position());
    block.instruction_count = static_cast<u32>(block.instructions.size());
    return block;
}

void expect_far_jit_matches_interpreter(GuestMemory& memory, const CpuState& start,
                                        u64 code_base, u64 stack_base, usize stack_size,
                                        const std::vector<u8>& code, DecodeMode mode) {
    for (usize i = 0; i < code.size(); ++i) {
        ASSERT_TRUE(memory.write_u8(code_base + i, code[i]).is_ok());
    }
    std::vector<u8> stack_snap(stack_size);
    ASSERT_TRUE(memory.read_bytes(stack_base, stack_snap.data(), stack_size));

    CpuState icpu = start;
    icpu.rip = code_base;
    {
        Interpreter interp(icpu, memory);
        EXPECT_EQ(interp.step(), ExecResult::Branch);
    }
    std::vector<u8> interp_stack(stack_size);
    ASSERT_TRUE(memory.read_bytes(stack_base, interp_stack.data(), stack_size));

    ASSERT_TRUE(memory.write_bytes(stack_base, stack_snap.data(), stack_size));

    CpuState jcpu = start;
    jcpu.rip = code_base;
    BasicBlock block = decode_far_block(code.data(), code.size(), mode, code_base);
    ASSERT_GT(block.instruction_count, 0u);
    JitCompiler jit(memory);
    CompiledBlock* cb = jit.compile(block);
    ASSERT_NE(cb, nullptr);
    EXPECT_EQ(jit.execute(*cb, jcpu), JitExecResult::Continue);

    EXPECT_EQ(icpu.rip, jcpu.rip);
    EXPECT_EQ(icpu.seg.cs, jcpu.seg.cs);
    EXPECT_EQ(icpu.sp(), jcpu.sp());
    const int nregs = (mode == DecodeMode::Mode64) ? 16 : 8;
    for (int i = 0; i < nregs; ++i) {
        EXPECT_EQ(icpu.gpr.get64(static_cast<u8>(i)),
                  jcpu.gpr.get64(static_cast<u8>(i)))
            << "GPR" << i << " diverged";
    }
    std::vector<u8> jit_stack(stack_size);
    ASSERT_TRUE(memory.read_bytes(stack_base, jit_stack.data(), stack_size));
    EXPECT_EQ(interp_stack, jit_stack);
}

}  // namespace

TEST_F(StackInterp32Test, FarJmpPtr16_32JitMatchesInterpreter) {
    expect_far_jit_matches_interpreter(
        memory, cpu, CODE_BASE, STACK_BASE, STACK_SIZE,
        {0xEA, 0x78, 0x56, 0x34, 0x12, 0x00, 0x08}, DecodeMode::Mode32);
}

TEST_F(StackInterp32Test, FarCallPtr16_32JitMatchesInterpreter) {
    expect_far_jit_matches_interpreter(
        memory, cpu, CODE_BASE, STACK_BASE, STACK_SIZE,
        {0x9A, 0x00, 0x18, 0x00, 0x00, 0x23, 0x00}, DecodeMode::Mode32);
}

TEST_F(StackInterp32Test, FarJmpM16_32JitMatchesInterpreter) {
    ASSERT_TRUE(memory.write_u32(STACK_BASE, 0x2000).is_ok());
    ASSERT_TRUE(memory.write_u16(STACK_BASE + 4, 0x001B).is_ok());
    expect_far_jit_matches_interpreter(
        memory, cpu, CODE_BASE, STACK_BASE, STACK_SIZE,
        {0xFF, 0x2D, 0x00, 0x00, 0x01, 0x00}, DecodeMode::Mode32);
}

TEST_F(StackInterp32Test, FarCallM16_32JitMatchesInterpreter) {
    ASSERT_TRUE(memory.write_u32(STACK_BASE, 0x2000).is_ok());
    ASSERT_TRUE(memory.write_u16(STACK_BASE + 4, 0x001B).is_ok());
    expect_far_jit_matches_interpreter(
        memory, cpu, CODE_BASE, STACK_BASE, STACK_SIZE,
        {0xFF, 0x1D, 0x00, 0x00, 0x01, 0x00}, DecodeMode::Mode32);
}

TEST_F(StackInterp32Test, FarJmpPtr16_16JitMatchesInterpreter) {
    expect_far_jit_matches_interpreter(
        memory, cpu, CODE_BASE, STACK_BASE, STACK_SIZE,
        {0x66, 0xEA, 0x34, 0x12, 0x00, 0x08}, DecodeMode::Mode32);
}

TEST_F(StackInterpTest, FarCallM16_64JitMatchesInterpreter) {
    cpu.gpr.rax = STACK_BASE;
    ASSERT_TRUE(memory.write_u64(STACK_BASE, 0x1800).is_ok());
    ASSERT_TRUE(memory.write_u16(STACK_BASE + 8, 0x0033).is_ok());
    expect_far_jit_matches_interpreter(
        memory, cpu, CODE_BASE, STACK_BASE, STACK_SIZE,
        {0xFF, 0x18}, DecodeMode::Mode64);
}

TEST_F(StackInterpTest, FarJmpM16_64JitMatchesInterpreter) {
    cpu.gpr.rax = STACK_BASE;
    ASSERT_TRUE(memory.write_u64(STACK_BASE, 0x1800).is_ok());
    ASSERT_TRUE(memory.write_u16(STACK_BASE + 8, 0x0033).is_ok());
    expect_far_jit_matches_interpreter(
        memory, cpu, CODE_BASE, STACK_BASE, STACK_SIZE,
        {0xFF, 0x28}, DecodeMode::Mode64);
}

TEST_F(StackInterpTest, FarJmpM16_16JitMatchesInterpreter) {
    cpu.gpr.rax = STACK_BASE;
    ASSERT_TRUE(memory.write_u16(STACK_BASE, 0x1800).is_ok());
    ASSERT_TRUE(memory.write_u16(STACK_BASE + 2, 0x0033).is_ok());
    expect_far_jit_matches_interpreter(
        memory, cpu, CODE_BASE, STACK_BASE, STACK_SIZE,
        {0x66, 0xFF, 0x28}, DecodeMode::Mode64);
}

TEST_F(StackInterpTest, FarCallM16_16JitMatchesInterpreter) {
    cpu.gpr.rax = STACK_BASE;
    ASSERT_TRUE(memory.write_u16(STACK_BASE, 0x1800).is_ok());
    ASSERT_TRUE(memory.write_u16(STACK_BASE + 2, 0x0033).is_ok());
    expect_far_jit_matches_interpreter(
        memory, cpu, CODE_BASE, STACK_BASE, STACK_SIZE,
        {0x66, 0xFF, 0x18}, DecodeMode::Mode64);
}
