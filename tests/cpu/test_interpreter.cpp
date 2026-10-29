// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#include <gtest/gtest.h>
#include <vector>
#include "core/cpu/interpreter/interpreter.hpp"
#include <vitreo/guest_sentinels.hpp>
#include "core/cpu/seh/seh32.hpp"

using namespace vitreo;
using namespace vitreo::cpu;

// Convenience register indices
constexpr u8 RAX = static_cast<u8>(RegIndex::RAX);
constexpr u8 RCX = static_cast<u8>(RegIndex::RCX);
constexpr u8 RDX = static_cast<u8>(RegIndex::RDX);
constexpr u8 RBX = static_cast<u8>(RegIndex::RBX);
constexpr u8 RSP = static_cast<u8>(RegIndex::RSP);
constexpr u8 RBP = static_cast<u8>(RegIndex::RBP);
constexpr u8 RSI = static_cast<u8>(RegIndex::RSI);
constexpr u8 RDI = static_cast<u8>(RegIndex::RDI);
constexpr u8 R8  = static_cast<u8>(RegIndex::R8);

// AL is index 0 in 8-bit encoding
constexpr u8 AL = 0;
constexpr u8 BL = 3;

class InterpreterTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        // Map 64KB of guest memory for tests at address 0x1000
        // Note: Use ReadWrite since interpreter doesn't execute native code
        auto map_result = m_memory->map_region(0x1000, 0x10000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(map_result);
        
        m_cpu = std::make_unique<CpuState>();
        m_cpu->init_long_mode();
        
        // Set initial stack pointer
        m_cpu->gpr.set64(RSP, 0x10000);
        
        // Set initial instruction pointer to start of code area
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
    
    ExecResult execute_n(u64 n) {
        Interpreter interp(*m_cpu, *m_memory);
        return interp.run(n);
    }
    
    std::unique_ptr<GuestMemory> m_memory;
    std::unique_ptr<CpuState> m_cpu;
};

// ============================================================================
// NOP tests
// ============================================================================

TEST_F(InterpreterTest, NopAdvancesIP) {
    u8 code[] = { 0x90 }; // NOP
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->rip, 0x1001);
}

TEST_F(InterpreterTest, SyscallCapturesReturnStateInLongMode) {
    const u8 code[] = {0x0F, 0x05};
    write_code(code, sizeof(code));
    m_cpu->flags.rflags = 0xA47;

    EXPECT_EQ(execute_one(), ExecResult::Syscall);
    EXPECT_EQ(m_cpu->rip, 0x1002u);
    EXPECT_EQ(m_cpu->gpr.rcx, 0x1002u);
    EXPECT_EQ(m_cpu->gpr.r11, 0xA47u);
}

TEST_F(InterpreterTest, SysenterReturnsSyscall) {
    const u8 code[] = {0x0F, 0x34};
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Syscall);
    EXPECT_EQ(m_cpu->rip, 0x1002u);
}

TEST_F(InterpreterTest, SysexitIsPrivileged) {
    const u8 code[] = {0x0F, 0x35};
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Exception);
}

TEST_F(InterpreterTest, MultipleNops) {
    u8 code[] = { 0x90, 0x90, 0x90 }; // NOP; NOP; NOP
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_n(3), ExecResult::Continue);
    EXPECT_EQ(m_cpu->rip, 0x1003);
}

TEST_F(InterpreterTest, XlatUsesEbxPlusUnsignedAlInProtectedMode) {
    m_cpu->init_protected_mode();
    m_cpu->rip = 0x1000;
    m_cpu->gpr.set32(RBX, 0x2000);
    m_cpu->gpr.set32(RAX, 0x12345608);
    m_cpu->flags.rflags = 0x8D7;
    const u64 flags_before = m_cpu->flags.rflags;
    ASSERT_TRUE(m_memory->write_u8(0x2008, 0xA7).is_ok());
    const u8 code[] = {0xD7};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(RAX), 0x123456A7u);
    EXPECT_EQ(m_cpu->flags.rflags, flags_before);
    EXPECT_EQ(m_cpu->rip, 0x1001u);
}

TEST_F(InterpreterTest, XlatAddressOverrideUsesBxInProtectedMode) {
    m_cpu->init_protected_mode();
    m_cpu->rip = 0x1000;
    m_cpu->gpr.set32(RBX, 0xCAFE2000);
    m_cpu->gpr.set8l(RAX, 3);
    ASSERT_TRUE(m_memory->write_u8(0x2003, 0x5C).is_ok());
    const u8 code[] = {0x67, 0xD7};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get8l(RAX), 0x5Cu);
    EXPECT_EQ(m_cpu->rip, 0x1002u);
}

TEST_F(InterpreterTest, XlatHonorsFsOverrideInLongMode) {
    m_cpu->gpr.set64(RBX, 0x20);
    m_cpu->gpr.set8l(RAX, 4);
    m_cpu->seg.fs_desc.base = 0x3000;
    ASSERT_TRUE(m_memory->write_u8(0x3024, 0xE1).is_ok());
    const u8 code[] = {0x64, 0xD7};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get8l(RAX), 0xE1u);
    EXPECT_EQ(m_cpu->rip, 0x1002u);
}

// ============================================================================
// MOV tests
// ============================================================================

TEST_F(InterpreterTest, MovR64Imm64) {
    // MOV RAX, 0x123456789ABCDEF0 (48 B8 F0 DE BC 9A 78 56 34 12)
    u8 code[] = { 0x48, 0xB8, 0xF0, 0xDE, 0xBC, 0x9A, 0x78, 0x56, 0x34, 0x12 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(RAX), 0x123456789ABCDEF0ULL);
}

TEST_F(InterpreterTest, MovR32Imm32ClearsUpper) {
    // Set upper bits first
    m_cpu->gpr.set64(RAX, 0xFFFFFFFF00000000ULL);
    
    // MOV EAX, 0x12345678 (B8 78 56 34 12)
    u8 code[] = { 0xB8, 0x78, 0x56, 0x34, 0x12 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // Upper 32 bits should be cleared
    EXPECT_EQ(m_cpu->gpr.get64(RAX), 0x12345678ULL);
}

TEST_F(InterpreterTest, MovR8Imm8) {
    // MOV AL, 0x42 (B0 42)
    u8 code[] = { 0xB0, 0x42 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get8l(AL), 0x42);
}

TEST_F(InterpreterTest, MovRegToReg) {
    m_cpu->gpr.set64(RBX, 0xDEADBEEF);
    
    // MOV RAX, RBX = 48 8B C3
    u8 code[] = { 0x48, 0x8B, 0xC3 }; // MOV RAX, RBX
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(RAX), 0xDEADBEEF);
}

// ============================================================================
// ADD tests
// ============================================================================

TEST_F(InterpreterTest, AddR64R64) {
    m_cpu->gpr.set64(RAX, 100);
    m_cpu->gpr.set64(RBX, 50);
    
    // ADD RAX, RBX (48 01 D8)
    u8 code[] = { 0x48, 0x01, 0xD8 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(RAX), 150);
}

TEST_F(InterpreterTest, AddSetsZeroFlag) {
    // ADD EAX, EBX (01 D8) - use 32-bit to get proper wrap
    m_cpu->gpr.set32(RAX, 5);
    m_cpu->gpr.set32(RBX, static_cast<u32>(-5));
    u8 code[] = { 0x01, 0xD8 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(RAX), 0);
    EXPECT_TRUE(m_cpu->flags.zf());
}

TEST_F(InterpreterTest, AddSetsCarryFlag) {
    m_cpu->gpr.set32(RAX, 0xFFFFFFFF);
    m_cpu->gpr.set32(RBX, 1);
    
    // ADD EAX, EBX (01 D8)
    u8 code[] = { 0x01, 0xD8 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(RAX), 0);
    EXPECT_TRUE(m_cpu->flags.cf());
}

TEST_F(InterpreterTest, AddAlImm8) {
    m_cpu->gpr.set8l(AL, 10);
    
    // ADD AL, 5 (04 05)
    u8 code[] = { 0x04, 0x05 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get8l(AL), 15);
}

// ============================================================================
// SUB tests
// ============================================================================

TEST_F(InterpreterTest, SubR64R64) {
    m_cpu->gpr.set64(RAX, 100);
    m_cpu->gpr.set64(RBX, 30);
    
    // SUB RAX, RBX (48 29 D8)
    u8 code[] = { 0x48, 0x29, 0xD8 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(RAX), 70);
}

TEST_F(InterpreterTest, SubSetsBorrowFlag) {
    m_cpu->gpr.set32(RAX, 5);
    m_cpu->gpr.set32(RBX, 10);
    
    // SUB EAX, EBX (29 D8)
    u8 code[] = { 0x29, 0xD8 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_TRUE(m_cpu->flags.cf()); // Borrow
}

// ============================================================================
// XOR tests
// ============================================================================

TEST_F(InterpreterTest, XorSelf) {
    m_cpu->gpr.set64(RAX, 0x12345678);
    
    // XOR EAX, EAX (31 C0)
    u8 code[] = { 0x31, 0xC0 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(RAX), 0);
    EXPECT_TRUE(m_cpu->flags.zf());
}

TEST_F(InterpreterTest, XorR64R64) {
    m_cpu->gpr.set64(RAX, 0xFF00FF00);
    m_cpu->gpr.set64(RBX, 0x0F0F0F0F);
    
    // XOR RAX, RBX (48 31 D8)
    u8 code[] = { 0x48, 0x31, 0xD8 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(RAX), 0xF00FF00F);
}

// ============================================================================
// CMP/TEST tests
// ============================================================================

TEST_F(InterpreterTest, CmpSetsFlags) {
    m_cpu->gpr.set64(RAX, 50);
    m_cpu->gpr.set64(RBX, 50);
    
    // CMP RAX, RBX (48 39 D8)
    u8 code[] = { 0x48, 0x39, 0xD8 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_TRUE(m_cpu->flags.zf()); // Equal
    EXPECT_EQ(m_cpu->gpr.get64(RAX), 50); // RAX unchanged
}

TEST_F(InterpreterTest, TestSetsFlags) {
    m_cpu->gpr.set64(RAX, 0);
    m_cpu->gpr.set64(RBX, 0xFF);
    
    // TEST RAX, RBX (48 85 D8)
    u8 code[] = { 0x48, 0x85, 0xD8 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_TRUE(m_cpu->flags.zf()); // 0 & 0xFF = 0
}

// ============================================================================
// PUSH/POP tests
// ============================================================================

TEST_F(InterpreterTest, PushR64) {
    m_cpu->gpr.set64(RAX, 0x123456789ABCDEF0ULL);
    u64 initial_sp = m_cpu->gpr.get64(RSP);
    
    // PUSH RAX (50)
    u8 code[] = { 0x50 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(RSP), initial_sp - 8);
    
    // Verify value on stack
    auto val = m_memory->read_u64(initial_sp - 8);
    ASSERT_TRUE(val);
    EXPECT_EQ(val.value(), 0x123456789ABCDEF0ULL);
}

TEST_F(InterpreterTest, PopR64) {
    u64 initial_sp = m_cpu->gpr.get64(RSP) - 8;
    m_cpu->gpr.set64(RSP, initial_sp);
    
    // Write value to stack
    auto write_result = m_memory->write_u64(initial_sp, 0xDEADBEEFCAFEBABEULL);
    ASSERT_TRUE(write_result);
    
    // POP RAX (58)
    u8 code[] = { 0x58 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(RSP), initial_sp + 8);
    EXPECT_EQ(m_cpu->gpr.get64(RAX), 0xDEADBEEFCAFEBABEULL);
}

TEST_F(InterpreterTest, PushPopRoundTrip) {
    m_cpu->gpr.set64(RBX, 0x42424242);
    u64 initial_sp = m_cpu->gpr.get64(RSP);
    
    // PUSH RBX; POP RAX (53 58)
    u8 code[] = { 0x53, 0x58 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_n(2), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(RSP), initial_sp);
    EXPECT_EQ(m_cpu->gpr.get64(RAX), 0x42424242);
}

// ============================================================================
// JMP tests
// ============================================================================

TEST_F(InterpreterTest, JmpRel8Forward) {
    // JMP +5 (EB 05), then some nops, then HLT
    u8 code[] = { 0xEB, 0x05, 0x90, 0x90, 0x90, 0x90, 0x90, 0xF4 };
    write_code(code, sizeof(code));
    
    auto result = execute_one();
    EXPECT_EQ(result, ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x1007); // 0x1000 + 2 (instr len) + 5 (offset)
}

TEST_F(InterpreterTest, JmpRel8Backward) {
    // Setup: start at 0x1005, jump back to 0x1000
    m_cpu->rip = 0x1005;
    
    // JMP -5 (EB FB)
    u8 code[16] = {};
    code[5] = 0xEB;
    code[6] = 0xFB; // -5
    write_code(code, sizeof(code));
    
    auto result = execute_one();
    EXPECT_EQ(result, ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x1002); // 0x1005 + 2 - 5
}

// ============================================================================
// Conditional jump tests
// ============================================================================

TEST_F(InterpreterTest, JzTaken) {
    m_cpu->flags.set_zf(true);
    
    // JZ +10 (74 0A)
    u8 code[] = { 0x74, 0x0A };
    write_code(code, sizeof(code));
    
    auto result = execute_one();
    EXPECT_EQ(result, ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x100C); // 0x1000 + 2 + 10
}

TEST_F(InterpreterTest, JzNotTaken) {
    m_cpu->flags.set_zf(false);
    
    // JZ +10 (74 0A)
    u8 code[] = { 0x74, 0x0A };
    write_code(code, sizeof(code));
    
    auto result = execute_one();
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(m_cpu->rip, 0x1002); // Just advances past instruction
}

TEST_F(InterpreterTest, JnzTaken) {
    m_cpu->flags.set_zf(false);
    
    // JNZ +5 (75 05)
    u8 code[] = { 0x75, 0x05 };
    write_code(code, sizeof(code));
    
    auto result = execute_one();
    EXPECT_EQ(result, ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x1007);
}

// ============================================================================
// CALL/RET tests
// ============================================================================

TEST_F(InterpreterTest, CallRel32) {
    u64 initial_sp = m_cpu->gpr.get64(RSP);
    
    // CALL +0x100 (E8 00 01 00 00)
    u8 code[] = { 0xE8, 0x00, 0x01, 0x00, 0x00 };
    write_code(code, sizeof(code));
    
    auto result = execute_one();
    EXPECT_EQ(result, ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x1105); // 0x1000 + 5 + 0x100
    EXPECT_EQ(m_cpu->gpr.get64(RSP), initial_sp - 8);
    
    // Return address should be pushed
    auto ret_addr = m_memory->read_u64(initial_sp - 8);
    ASSERT_TRUE(ret_addr);
    EXPECT_EQ(ret_addr.value(), 0x1005); // Address after CALL
}

TEST_F(InterpreterTest, RetNear) {
    // Setup: push return address
    u64 initial_sp = m_cpu->gpr.get64(RSP) - 8;
    m_cpu->gpr.set64(RSP, initial_sp);
    auto write_result = m_memory->write_u64(initial_sp, 0x2000);
    ASSERT_TRUE(write_result);
    
    // RET (C3)
    u8 code[] = { 0xC3 };
    write_code(code, sizeof(code));
    
    auto result = execute_one();
    EXPECT_EQ(result, ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x2000);
    EXPECT_EQ(m_cpu->gpr.get64(RSP), initial_sp + 8);
}

TEST_F(InterpreterTest, CallRetRoundTrip) {
    u64 initial_sp = m_cpu->gpr.get64(RSP);
    
    // Code at 0x1000: CALL +0x10 (5 bytes)
    // Code at 0x1005: <next instruction would be here>
    // Code at 0x1015: RET
    u8 code[32] = {};
    code[0] = 0xE8; code[1] = 0x10; code[2] = 0x00; code[3] = 0x00; code[4] = 0x00; // CALL +16
    code[21] = 0xC3; // RET at offset 0x15 (0x1015)
    write_code(code, sizeof(code));
    
    // Execute CALL
    auto result = execute_one();
    EXPECT_EQ(result, ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x1015);
    
    // Execute RET
    result = execute_one();
    EXPECT_EQ(result, ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x1005); // Returns to after CALL
    EXPECT_EQ(m_cpu->gpr.get64(RSP), initial_sp);
}

// ============================================================================
// LEA tests
// ============================================================================

TEST_F(InterpreterTest, LeaSimple) {
    m_cpu->gpr.set64(RBX, 0x1000);
    
    // LEA RAX, [RBX+0x10] (48 8D 43 10)
    u8 code[] = { 0x48, 0x8D, 0x43, 0x10 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(RAX), 0x1010);
}

// ============================================================================
// MOVZX/MOVSX tests
// ============================================================================

TEST_F(InterpreterTest, MovzxR32Rm8) {
    m_cpu->gpr.set64(RAX, 0xFFFFFFFFFFFFFFFFULL);
    m_cpu->gpr.set8l(BL, 0x42);
    
    // MOVZX EAX, BL (0F B6 C3)
    u8 code[] = { 0x0F, 0xB6, 0xC3 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(RAX), 0x42);
}

TEST_F(InterpreterTest, MovsxR64Rm8) {
    m_cpu->gpr.set8l(BL, 0x80); // -128 in signed
    
    // MOVSX RAX, BL (48 0F BE C3)
    u8 code[] = { 0x48, 0x0F, 0xBE, 0xC3 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // Should sign-extend to 0xFFFFFFFFFFFFFF80
    EXPECT_EQ(m_cpu->gpr.get64(RAX), 0xFFFFFFFFFFFFFF80ULL);
}

TEST_F(InterpreterTest, MovsxR64FromUnmappedMemoryFaults) {
    m_cpu->gpr.set64(RBX, 0);

    // MOVSX RCX, byte ptr [RBX+0x17] (48 0F BE 4B 17)
    u8 code[] = { 0x48, 0x0F, 0xBE, 0x4B, 0x17 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Exception);
}

// ============================================================================
// Special instructions
// ============================================================================

TEST_F(InterpreterTest, Int3) {
    // INT 3 (CC)
    u8 code[] = { 0xCC };
    write_code(code, sizeof(code));
    
    Interpreter interp(*m_cpu, *m_memory);
    auto result = interp.step();
    
    EXPECT_EQ(result, ExecResult::Interrupt);
    EXPECT_EQ(interp.context().interrupt_num, 3);
    EXPECT_EQ(interp.context().exception_addr, 0x1000u);
    EXPECT_EQ(m_cpu->rip, 0x1001u);
}

TEST_F(InterpreterTest, Int29FastFail) {
    u8 code[] = { 0xCD, 0x29 };
    write_code(code, sizeof(code));

    Interpreter interp(*m_cpu, *m_memory);
    auto result = interp.step();

    EXPECT_EQ(result, ExecResult::Interrupt);
    EXPECT_EQ(interp.context().interrupt_num, 0x29);
}

TEST_F(InterpreterTest, Int29FastFailProtectedModeKeepsEcxCode) {
    // Learn: /cpp/intrinsics/fastfail — x86 INT 0x29 takes the code in ECX
    // and does not invoke exception handlers. The interpreter must surface
    // the same Interrupt 0x29 that long mode already did.
    m_cpu->init_protected_mode();
    m_cpu->rip = 0x1000;
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RCX), 2);
    const u8 code[] = {0xCD, 0x29};
    write_code(code, sizeof(code));

    Interpreter interp(*m_cpu, *m_memory);
    auto result = interp.step();

    EXPECT_EQ(result, ExecResult::Interrupt);
    EXPECT_EQ(interp.context().interrupt_num, 0x29);
    EXPECT_EQ(m_cpu->gpr.get32(static_cast<u8>(RegIndex::RCX)), 2u);
    EXPECT_EQ(m_cpu->rip, 0x1000u);
}

TEST_F(InterpreterTest, IntoOfClearContinues) {
    m_cpu->init_protected_mode();
    m_cpu->rip = 0x1000;
    m_cpu->flags.set_of(false);
    const u8 code[] = {0xCE};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->rip, 0x1001u);
}

TEST_F(InterpreterTest, IntoOfSetRaisesInterrupt4) {
    m_cpu->init_protected_mode();
    m_cpu->rip = 0x1000;
    m_cpu->flags.set_of(true);
    const u8 code[] = {0xCE};
    write_code(code, sizeof(code));

    Interpreter interp(*m_cpu, *m_memory);
    auto result = interp.step();

    EXPECT_EQ(result, ExecResult::Interrupt);
    EXPECT_EQ(interp.context().interrupt_num, 4);
}

TEST_F(InterpreterTest, PauseContinues) {
    const u8 code[] = {0xF3, 0x90};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->rip, 0x1002u);
}

TEST_F(InterpreterTest, HltIsPrivilegedInUserMode) {
    // HLT (F4) raises #GP outside CPL 0. Vitreo is a user-mode runtime, so it
    // must surface the Windows privileged-instruction exception.
    u8 code[] = { 0xF4 };
    write_code(code, sizeof(code));

    Interpreter interp(*m_cpu, *m_memory);
    EXPECT_EQ(interp.step(), ExecResult::Exception);
    EXPECT_EQ(interp.context().exception, CpuException::PrivilegedInstruction);
    EXPECT_EQ(cpu_exception_to_status(interp.context().exception),
              STATUS_PRIVILEGED_INSTRUCTION);
}

TEST_F(InterpreterTest, WbinvdIsPrivilegedInUserMode) {
    // Intel SDM Vol. 2D: WBINVD (0F 09) requires CPL 0 in protected and
    // 64-bit mode. Windows reports a user-mode attempt as 0xC0000096.
    const u8 code[] = {0x0F, 0x09};
    write_code(code, sizeof(code));

    Interpreter interp(*m_cpu, *m_memory);
    EXPECT_EQ(interp.step(), ExecResult::Exception);
    EXPECT_EQ(interp.context().exception, CpuException::PrivilegedInstruction);
    EXPECT_EQ(cpu_exception_to_status(interp.context().exception),
              STATUS_PRIVILEGED_INSTRUCTION);
}

TEST_F(InterpreterTest, Ud2) {
    // UD2 (0F 0B)
    u8 code[] = { 0x0F, 0x0B };
    write_code(code, sizeof(code));
    
    Interpreter interp(*m_cpu, *m_memory);
    auto result = interp.step();
    
    EXPECT_EQ(result, ExecResult::Exception);
    EXPECT_EQ(interp.context().exception, CpuException::InvalidOpcode);
}

TEST_F(InterpreterTest, Cpuid) {
    m_cpu->gpr.set32(RAX, 1);
    
    // CPUID (0F A2)
    u8 code[] = { 0x0F, 0xA2 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // Just verify it doesn't crash; values are 0
}

TEST_F(InterpreterTest, Rdtsc) {
    // RDTSC (0F 31)
    u8 code[] = { 0x0F, 0x31 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // Just verify EAX:EDX has some value
    u64 tsc = (static_cast<u64>(m_cpu->gpr.get32(RDX)) << 32) | 
              m_cpu->gpr.get32(RAX);
    EXPECT_GT(tsc, 0);
}

TEST_F(InterpreterTest, RdtscpWritesEdxEaxEcx) {
    const u8 code[] = {0x0F, 0x01, 0xF9};
    write_code(code, sizeof(code));
    m_cpu->gpr.set64(RCX, 0xFFFFFFFFFFFFFFFFull);

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    const u64 tsc = (static_cast<u64>(m_cpu->gpr.get32(RDX)) << 32) |
                    m_cpu->gpr.get32(RAX);
    EXPECT_GT(tsc, 0u);
    EXPECT_EQ(m_cpu->gpr.get32(RCX), 0u);
    EXPECT_EQ(m_cpu->gpr.get64(RCX), 0u);
    EXPECT_EQ(m_cpu->rip, 0x1003u);
}

// ============================================================================
// Memory access tests
// ============================================================================

TEST_F(InterpreterTest, MovToMemory) {
    m_cpu->gpr.set64(RAX, 0xDEADBEEF);
    m_cpu->gpr.set64(RBX, 0x1100); // Address to write to
    
    // MOV [RBX], EAX (89 03)
    u8 code[] = { 0x89, 0x03 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    
    auto val = m_memory->read_u32(0x1100);
    ASSERT_TRUE(val);
    EXPECT_EQ(val.value(), 0xDEADBEEF);
}

TEST_F(InterpreterTest, MovFromMemory) {
    // Write value to memory first
    auto write_result = m_memory->write_u64(0x1100, 0x123456789ABCDEF0ULL);
    ASSERT_TRUE(write_result);
    
    m_cpu->gpr.set64(RBX, 0x1100);
    
    // MOV RAX, [RBX] (48 8B 03)
    u8 code[] = { 0x48, 0x8B, 0x03 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(RAX), 0x123456789ABCDEF0ULL);
}

// ============================================================================
// Interpreter run() tests
// ============================================================================

TEST_F(InterpreterTest, RunUntilHalt) {
    // NOP; NOP; NOP; HLT
    u8 code[] = { 0x90, 0x90, 0x90, 0xF4 };
    write_code(code, sizeof(code));
    
    Interpreter interp(*m_cpu, *m_memory);
    auto result = interp.run();
    
    EXPECT_EQ(result, ExecResult::Exception);
    EXPECT_EQ(interp.context().exception, CpuException::PrivilegedInstruction);
    EXPECT_EQ(interp.instruction_count(), 4);
}

TEST_F(InterpreterTest, RunWithMaxInstructions) {
    // Infinite loop: JMP -2
    u8 code[] = { 0xEB, 0xFE };
    write_code(code, sizeof(code));
    
    Interpreter interp(*m_cpu, *m_memory);
    auto result = interp.run(10);
    
    // Should stop after 10 iterations
    EXPECT_EQ(interp.instruction_count(), 10);
}

// ============================================================================
// Near branches to a NON-CANONICAL target raise #GP(0) at the branch
//
// Intel SDM Vol. 2 JMP/CALL/RET/Jcc (64-bit mode), measured on Windows
// (tools/oracle_probe/noncanonical_branch_probe.expected): the fault is
// reported AT the branch with nothing committed -- RET has not popped, CALL
// has not pushed -- as STATUS_ACCESS_VIOLATION, ExceptionInformation[0] = 0,
// [1] = 0xFFFFFFFFFFFFFFFF. A Jcc not taken never examines its target, and a
// CANONICAL target that is unmapped is not the branch's fault at all (it is
// the fetch of the target, after the branch has committed).
// ============================================================================

namespace {

struct BranchFault {
    ExecResult result;
    u32 status;
    u32 error;
    u64 addr;
};

BranchFault step_branch(CpuState& cpu, GuestMemory& mem) {
    Interpreter interp(cpu, mem);
    const ExecResult r = interp.step();
    const ExecContext& ctx = interp.context();
    return {r, cpu_exception_to_status(ctx.exception), ctx.exception_error,
            ctx.exception_addr};
}

void expect_gp_at_branch(const BranchFault& f) {
    EXPECT_EQ(f.result, ExecResult::Exception);
    EXPECT_EQ(f.status, 0xC0000005u);
    EXPECT_EQ(f.error, 0u);
    EXPECT_EQ(f.addr, ~u64{0});
}

}  // namespace

TEST_F(InterpreterTest, NearBranchToNonCanonicalTargetIsGpAtTheBranch) {
    constexpr u64 kRsp = 0x8000;
    constexpr u64 kData = 0x3000;
    constexpr u64 kCanary = 0x5A5A'5A5A'5A5A'5A5Aull;
    struct Form {
        const char* name;
        std::vector<u8> code;
        bool target_on_stack;  // RET: the target is the popped slot
    };
    const Form forms[] = {
        {"ret", {0xC3}, true},
        {"ret imm16", {0xC2, 0x10, 0x00}, true},
        {"jmp rax", {0xFF, 0xE0}, false},
        {"jmp [rbx]", {0xFF, 0x23}, false},
        {"call rax", {0xFF, 0xD0}, false},
        {"call [rbx]", {0xFF, 0x13}, false},
        {"rex.w jmp rax", {0x48, 0xFF, 0xE0}, false},
    };
    for (const u64 target : {0x0000'8000'0000'0000ull, 0xFFFF'7FFF'FFFF'FFFFull}) {
        for (const Form& f : forms) {
            SCOPED_TRACE(::testing::Message() << f.name << " -> 0x" << std::hex
                                              << target);
            write_code(f.code.data(), f.code.size());
            m_cpu->rip = 0x1000;
            m_cpu->gpr.set64(RSP, kRsp);
            m_cpu->gpr.set64(RAX, target);
            m_cpu->gpr.set64(RBX, kData);
            ASSERT_TRUE(m_memory->write_u64(kData, target));
            ASSERT_TRUE(m_memory->write_u64(kRsp, f.target_on_stack ? target : 0));
            ASSERT_TRUE(m_memory->write_u64(kRsp - 8, kCanary));
            expect_gp_at_branch(step_branch(*m_cpu, *m_memory));
            EXPECT_EQ(m_cpu->rip, 0x1000u) << "reported at the branch";
            EXPECT_EQ(m_cpu->gpr.rsp, kRsp) << "nothing popped or pushed";
            EXPECT_EQ(m_memory->read_u64(kRsp - 8).value(), kCanary)
                << "a #GP CALL writes no return address";
        }
    }
}

TEST_F(InterpreterTest, Rel32BranchToNonCanonicalTargetIsGpAtTheBranch) {
    // Only code at the very top of the user range reaches a non-canonical
    // target with rel32 (the probe runs these from the page below
    // 0x7FFFFFFF0000).
    constexpr u64 kTop = 0x0000'7FFF'FFFE'0000ull;
    ASSERT_TRUE(m_memory->map_region(kTop, 0x10000, MemoryProtection::ReadWrite));
    constexpr u64 kSite = kTop + 0x10000 - 0x100;
    constexpr u64 kTarget = 0x0000'8000'0000'0000ull;
    struct Form {
        const char* name;
        std::vector<u8> op;
        bool zf;
        bool faults;
    };
    const Form forms[] = {
        {"jmp rel32", {0xE9}, false, true},
        {"call rel32", {0xE8}, false, true},
        {"jz rel32 taken", {0x0F, 0x84}, true, true},
        {"jnz rel32 not taken", {0x0F, 0x85}, true, false},
    };
    for (const Form& f : forms) {
        SCOPED_TRACE(f.name);
        std::vector<u8> code = f.op;
        const u64 next = kSite + code.size() + 4;
        const u32 rel = static_cast<u32>(kTarget - next);
        for (int i = 0; i < 4; ++i) code.push_back(static_cast<u8>(rel >> (8 * i)));
        ASSERT_TRUE(m_memory->write_bytes(kSite, code.data(), code.size()));
        m_cpu->rip = kSite;
        m_cpu->gpr.set64(RSP, 0x8000);
        m_cpu->flags.rflags = f.zf ? 0x246u : 0x202u;
        const BranchFault r = step_branch(*m_cpu, *m_memory);
        if (f.faults) {
            expect_gp_at_branch(r);
            EXPECT_EQ(m_cpu->rip, kSite);
            EXPECT_EQ(m_cpu->gpr.rsp, 0x8000u);
        } else {
            EXPECT_EQ(r.result, ExecResult::Continue);
            EXPECT_EQ(m_cpu->rip, next);
        }
    }
}

TEST_F(InterpreterTest, NearBranchToCanonicalUnmappedTargetCommits) {
    // The contrast row: a canonical target is no #GP. The branch commits and
    // the fault, if any, belongs to the FETCH at the target.
    constexpr u64 kTarget = 0x0000'7FFF'FFFF'0000ull;
    const u8 code[] = {0xC2, 0x10, 0x00};  // ret 16
    write_code(code, sizeof(code));
    m_cpu->gpr.set64(RSP, 0x8000);
    ASSERT_TRUE(m_memory->write_u64(0x8000, kTarget));
    const BranchFault r = step_branch(*m_cpu, *m_memory);
    EXPECT_EQ(r.result, ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, kTarget);
    EXPECT_EQ(m_cpu->gpr.rsp, 0x8000u + 8u + 16u);
}

// Vitreo's host return sentinels are non-canonical BY DESIGN
// (include/vitreo/guest_sentinels.hpp): a guest callback or process entry
// returns to one, and the run loop recognises it at the RIP. RET to a sentinel
// is therefore a completed branch, never the #GP(0) above.
TEST_F(InterpreterTest, RetToHostReturnSentinelIsABranchNotAGp) {
    constexpr u64 kRsp = 0x8000;
    for (const u64 sentinel : {kCallbackReturnSentinel, kProcessReturnSentinel}) {
        SCOPED_TRACE(::testing::Message() << "ret -> 0x" << std::hex << sentinel);
        ASSERT_NE(static_cast<u64>(static_cast<i64>(sentinel << 16) >> 16),
                  sentinel) << "the sentinel is meant to be non-canonical";
        const u8 code[] = {0xC3};  // ret
        write_code(code, sizeof(code));
        m_cpu->rip = 0x1000;
        m_cpu->gpr.set64(RSP, kRsp);
        ASSERT_TRUE(m_memory->write_u64(kRsp, sentinel));
        const BranchFault r = step_branch(*m_cpu, *m_memory);
        EXPECT_EQ(r.result, ExecResult::Branch);
        EXPECT_EQ(m_cpu->rip, sentinel);
        EXPECT_EQ(m_cpu->gpr.rsp, kRsp + 8u) << "the slot was popped";
    }
}

// LOOP/LOOPcc/JRCXZ (Intel SDM Vol. 2): a taken branch to a non-canonical
// target is #GP(0) at the instruction with nothing committed -- LOOP's RCX
// decrement included. Only code on the last user page reaches one with rel8.
TEST_F(InterpreterTest, LoopToNonCanonicalTargetIsGpAndKeepsRcx) {
    constexpr u64 kPage = 0x0000'7FFF'FFFF'F000ull;
    ASSERT_TRUE(m_memory->map_region(kPage, 0x1000, MemoryProtection::ReadWrite));
    constexpr u64 kSite = kPage + 0xFF0;  // next = kSite + 2, +0x7F crosses 2^47
    struct Form {
        const char* name;
        u8 op;
        u64 rcx;
        bool zf;
        bool taken;
    };
    const Form forms[] = {
        {"loop taken", 0xE2, 5, false, true},
        {"loop not taken (rcx 1)", 0xE2, 1, false, false},
        {"loope taken", 0xE1, 5, true, true},
        {"loopne taken", 0xE0, 5, false, true},
        {"jrcxz taken", 0xE3, 0, false, true},
        {"jrcxz not taken", 0xE3, 7, false, false},
    };
    for (const Form& f : forms) {
        SCOPED_TRACE(f.name);
        const u8 code[] = {f.op, 0x7F};
        ASSERT_TRUE(m_memory->write_bytes(kSite, code, sizeof(code)));
        m_cpu->rip = kSite;
        m_cpu->gpr.set64(RSP, 0x8000);
        m_cpu->gpr.set64(RCX, f.rcx);
        m_cpu->flags.rflags = f.zf ? 0x246u : 0x202u;
        const BranchFault r = step_branch(*m_cpu, *m_memory);
        if (f.taken) {
            expect_gp_at_branch(r);
            EXPECT_EQ(m_cpu->rip, kSite) << "reported at the instruction";
            EXPECT_EQ(m_cpu->gpr.get64(RCX), f.rcx) << "RCX not decremented";
        } else {
            EXPECT_NE(r.result, ExecResult::Exception);
            EXPECT_EQ(m_cpu->rip, kSite + 2u);
            EXPECT_EQ(m_cpu->gpr.get64(RCX), f.op == 0xE3 ? f.rcx : f.rcx - 1u);
        }
    }
}

// ============================================================================
// 32-bit mode tests
// ============================================================================

class Interpreter32Test : public ::testing::Test {
protected:
    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        auto map_result = m_memory->map_region(0x1000, 0x10000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(map_result);
        
        m_cpu = std::make_unique<CpuState>();
        m_cpu->init_protected_mode();
        
        // Stack at end of region (0x1000 + 0x10000 = 0x11000 is first invalid addr)
        m_cpu->gpr.set32(RSP, 0x10FF0);
        m_cpu->rip = 0x1000;
    }
    
    void write_code(const u8* code, usize len) {
        auto result = m_memory->write_bytes(0x1000, code, len);
        ASSERT_TRUE(result);
    }
    
    std::unique_ptr<GuestMemory> m_memory;
    std::unique_ptr<CpuState> m_cpu;
};

TEST_F(Interpreter32Test, MovR32Imm32) {
    // MOV EAX, 0x12345678 (B8 78 56 34 12)
    u8 code[] = { 0xB8, 0x78, 0x56, 0x34, 0x12 };
    write_code(code, sizeof(code));
    
    Interpreter interp(*m_cpu, *m_memory);
    EXPECT_EQ(interp.step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(RAX), 0x12345678);
}

TEST_F(Interpreter32Test, PushPop32) {
    m_cpu->gpr.set32(RAX, 0xCAFEBABE);
    u32 initial_sp = m_cpu->gpr.get32(RSP);
    
    // PUSH EAX; POP EBX (50 5B)
    u8 code[] = { 0x50, 0x5B };
    write_code(code, sizeof(code));
    
    Interpreter interp(*m_cpu, *m_memory);
    EXPECT_EQ(interp.run(2), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(RBX), 0xCAFEBABE);
    EXPECT_EQ(m_cpu->gpr.get32(RSP), initial_sp);
}

TEST_F(Interpreter32Test, PushPopEsPreservesFourByteStackSlot) {
    const u32 initial_sp = m_cpu->gpr.get32(RSP);
    m_cpu->seg.es = 0x23;
    ASSERT_TRUE(m_memory->write_u32(initial_sp - 4, 0xAABBCCDD));
    u8 code[] = {0x06, 0x07};
    write_code(code, sizeof(code));

    Interpreter interp(*m_cpu, *m_memory);
    ASSERT_EQ(interp.step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(RSP), initial_sp - 4);
    EXPECT_EQ(m_memory->read_u32(initial_sp - 4).value(), 0xAABB0023u);

    m_cpu->seg.es = 0;
    ASSERT_EQ(interp.step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->seg.es, 0x23);
    EXPECT_EQ(m_cpu->gpr.get32(RSP), initial_sp);
}
