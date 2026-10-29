// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Indirect control flow instruction tests — CPU-022

#include <gtest/gtest.h>
#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"

using namespace vitreo;
using namespace vitreo::cpu;

// ============================================================================
// Decoder Tests
// ============================================================================

static DecodedInstruction decode(const std::initializer_list<u8>& bytes, DecodeMode mode = DecodeMode::Mode64) {
    std::vector<u8> data(bytes);
    MemoryByteReader reader(data.data(), data.size());
    DecodedInstruction inst;
    DecodeError err = decode_instruction(reader, mode, inst);
    EXPECT_EQ(err, DecodeError::Success) << "Decode failed";
    return inst;
}

// ----------------------------------------------------------------------------
// LEAVE (C9)
// ----------------------------------------------------------------------------

TEST(IndirectCtrlDecode, Leave) {
    auto inst = decode({0xC9});
    EXPECT_EQ(inst.opcode, Opcode::LEAVE);
    EXPECT_EQ(inst.length, 1);
    EXPECT_EQ(inst.operand_count, 0);
}

// ----------------------------------------------------------------------------
// ENTER (C8 imm16 imm8)
// ----------------------------------------------------------------------------

TEST(IndirectCtrlDecode, EnterBasic) {
    // C8 20 00 00 — ENTER 0x0020, 0
    auto inst = decode({0xC8, 0x20, 0x00, 0x00});
    EXPECT_EQ(inst.opcode, Opcode::ENTER);
    EXPECT_EQ(inst.length, 4);
    EXPECT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Imm16);
    EXPECT_EQ(inst.operands[0].imm, 0x20);
    EXPECT_EQ(inst.operands[1].type, OperandType::Imm8);
    EXPECT_EQ(inst.operands[1].imm, 0);
}

TEST(IndirectCtrlDecode, EnterLargeFrame) {
    // C8 00 01 00 — ENTER 0x0100, 0
    auto inst = decode({0xC8, 0x00, 0x01, 0x00});
    EXPECT_EQ(inst.opcode, Opcode::ENTER);
    EXPECT_EQ(inst.operands[0].imm, 0x100);
    EXPECT_EQ(inst.operands[1].imm, 0);
}

// ----------------------------------------------------------------------------
// CALL r/m64 (FF /2) — Group 5
// ----------------------------------------------------------------------------

TEST(IndirectCtrlDecode, CallRax) {
    // FF D0 — CALL RAX (mod=11 reg=010 rm=000)
    auto inst = decode({0xFF, 0xD0});
    EXPECT_EQ(inst.opcode, Opcode::CALL_RM64);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operand_count, 1);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 0); // RAX
}

TEST(IndirectCtrlDecode, CallRcx) {
    // FF D1 — CALL RCX
    auto inst = decode({0xFF, 0xD1});
    EXPECT_EQ(inst.opcode, Opcode::CALL_RM64);
    EXPECT_EQ(inst.operands[0].reg, 1); // RCX
}

// ----------------------------------------------------------------------------
// JMP r/m64 (FF /4) — Group 5
// ----------------------------------------------------------------------------

TEST(IndirectCtrlDecode, JmpRax) {
    // FF E0 — JMP RAX (mod=11 reg=100 rm=000)
    auto inst = decode({0xFF, 0xE0});
    EXPECT_EQ(inst.opcode, Opcode::JMP_RM64);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operand_count, 1);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 0); // RAX
}

// ============================================================================
// Interpreter Tests
// ============================================================================

class IndirectCtrlTest : public ::testing::Test {
protected:
    static constexpr u64 CODE_BASE = 0x1000;
    static constexpr u64 STACK_TOP = 0x11000;

    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        // Code + data region
        m_memory->map_region(0x1000, 0x10000, MemoryProtection::ReadWrite);
        // Stack region
        m_memory->map_region(0x10000, 0x2000, MemoryProtection::ReadWrite);

        m_cpu = std::make_unique<CpuState>();
        m_cpu->init_long_mode();
        m_cpu->rip = CODE_BASE;
        m_cpu->set_sp(STACK_TOP);
    }

    void write_code(const u8* code, usize len) {
        m_memory->write_bytes(m_cpu->rip, code, len);
    }

    void write_code_at(u64 addr, const u8* code, usize len) {
        m_memory->write_bytes(addr, code, len);
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
// CALL indirect tests
// ============================================================================

TEST_F(IndirectCtrlTest, CallRax_PushesReturnAndJumps) {
    // FF D0 — CALL RAX
    // RAX = 0x5000, instruction at 0x1000 length 2
    // Should push 0x1002 and jump to 0x5000
    m_cpu->gpr.rax = 0x5000;
    u8 code[] = { 0xFF, 0xD0 };
    write_code(code, sizeof(code));

    u64 sp_before = m_cpu->sp();
    auto result = execute_one();

    EXPECT_EQ(result, ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x5000u);
    EXPECT_EQ(m_cpu->sp(), sp_before - 8);
    EXPECT_EQ(read_stack_u64(m_cpu->sp()), 0x1002u); // return addr
}

TEST_F(IndirectCtrlTest, CallMemRipRelative) {
    // FF 15 xx xx xx xx — CALL [RIP+disp32]
    // RIP-relative: EA = next_rip + disp32 (next_rip = 0x1000 + 6 = 0x1006)
    // Want to read pointer from 0x2000: disp = 0x2000 - 0x1006 = 0x0FFA
    u64 target_addr = 0x7000;
    m_memory->write_u64(0x2000, target_addr);

    u8 code[] = { 0xFF, 0x15, 0xFA, 0x0F, 0x00, 0x00 };
    write_code(code, sizeof(code));

    u64 sp_before = m_cpu->sp();
    auto result = execute_one();

    EXPECT_EQ(result, ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x7000u);
    EXPECT_EQ(m_cpu->sp(), sp_before - 8);
    EXPECT_EQ(read_stack_u64(m_cpu->sp()), 0x1006u); // return addr
}

// ============================================================================
// JMP indirect tests
// ============================================================================

TEST_F(IndirectCtrlTest, JmpRax_NoPush) {
    // FF E0 — JMP RAX
    m_cpu->gpr.rax = 0x3000;
    u8 code[] = { 0xFF, 0xE0 };
    write_code(code, sizeof(code));

    u64 sp_before = m_cpu->sp();
    auto result = execute_one();

    EXPECT_EQ(result, ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x3000u);
    EXPECT_EQ(m_cpu->sp(), sp_before); // no push
}

TEST_F(IndirectCtrlTest, JmpMemRipRelative) {
    // FF 25 xx xx xx xx — JMP [RIP+disp32]
    // RIP-relative: EA = next_rip + disp32 (next_rip = 0x1000 + 6 = 0x1006)
    // Want to read pointer from 0x2000: disp = 0x2000 - 0x1006 = 0x0FFA
    u64 target_addr = 0x9000;
    m_memory->write_u64(0x2000, target_addr);

    u8 code[] = { 0xFF, 0x25, 0xFA, 0x0F, 0x00, 0x00 };
    write_code(code, sizeof(code));

    u64 sp_before = m_cpu->sp();
    auto result = execute_one();

    EXPECT_EQ(result, ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x9000u);
    EXPECT_EQ(m_cpu->sp(), sp_before); // no push
}

// ============================================================================
// LEAVE tests
// ============================================================================

TEST_F(IndirectCtrlTest, Leave_RestoresRspAndRbp) {
    // Set up a stack frame:
    // RBP points into the stack, saved old RBP is on stack
    u64 old_rbp = 0x10F00;
    u64 frame_base = STACK_TOP - 0x40;

    m_cpu->gpr.rbp = frame_base;
    m_cpu->set_sp(frame_base - 0x20); // RSP below RBP (local vars)
    m_memory->write_u64(frame_base, old_rbp); // saved RBP on stack

    // C9 — LEAVE
    u8 code[] = { 0xC9 };
    write_code(code, sizeof(code));

    auto result = execute_one();

    EXPECT_EQ(result, ExecResult::Continue);
    // RSP = old RBP + 8 (after pop)
    EXPECT_EQ(m_cpu->sp(), frame_base + 8);
    // RBP = the value that was saved on stack
    EXPECT_EQ(m_cpu->gpr.rbp, old_rbp);
}

TEST_F(IndirectCtrlTest, LeaveThenRet_Epilogue) {
    // Simulate function epilogue: LEAVE + RET
    // Setup: function was entered with CALL, RBP frame was built
    u64 return_addr = 0x4000;
    u64 old_rbp = 0x10E00;
    u64 frame_base = STACK_TOP - 0x40;

    // Stack layout (high to low):
    //   frame_base + 8: return address
    //   frame_base:     saved old RBP
    //   (below):        local variables
    m_memory->write_u64(frame_base + 8, return_addr);
    m_memory->write_u64(frame_base, old_rbp);

    m_cpu->gpr.rbp = frame_base;
    m_cpu->set_sp(frame_base - 0x20); // RSP below frame

    // LEAVE at 0x1000, RET at 0x1001
    u8 code[] = { 0xC9, 0xC3 };
    write_code(code, sizeof(code));

    // Execute LEAVE
    auto result = execute_one();
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.rbp, old_rbp);
    EXPECT_EQ(m_cpu->sp(), frame_base + 8);

    // IP advanced to RET (0x1001)
    EXPECT_EQ(m_cpu->rip, CODE_BASE + 1);

    // Execute RET
    result = execute_one();
    EXPECT_EQ(result, ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, return_addr);
    EXPECT_EQ(m_cpu->sp(), frame_base + 16);
}

// ============================================================================
// ENTER tests
// ============================================================================

TEST_F(IndirectCtrlTest, Enter_CreatesFrame) {
    // C8 20 00 00 — ENTER 0x0020, 0
    // Should: PUSH RBP, MOV RBP RSP, SUB RSP 0x20
    u64 old_rbp = 0xDEAD;
    m_cpu->gpr.rbp = old_rbp;

    u8 code[] = { 0xC8, 0x20, 0x00, 0x00 };
    write_code(code, sizeof(code));

    u64 sp_before = m_cpu->sp();
    auto result = execute_one();

    EXPECT_EQ(result, ExecResult::Continue);
    // Old RBP was pushed
    EXPECT_EQ(read_stack_u64(sp_before - 8), old_rbp);
    // RBP = SP after push (sp_before - 8)
    EXPECT_EQ(m_cpu->gpr.rbp, sp_before - 8);
    // RSP = RBP - 0x20
    EXPECT_EQ(m_cpu->sp(), sp_before - 8 - 0x20);
}

TEST_F(IndirectCtrlTest, Enter_ZeroFrameSize) {
    // C8 00 00 00 — ENTER 0, 0
    u64 old_rbp = 0xBEEF;
    m_cpu->gpr.rbp = old_rbp;

    u8 code[] = { 0xC8, 0x00, 0x00, 0x00 };
    write_code(code, sizeof(code));

    u64 sp_before = m_cpu->sp();
    auto result = execute_one();

    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(read_stack_u64(sp_before - 8), old_rbp);
    EXPECT_EQ(m_cpu->gpr.rbp, sp_before - 8);
    EXPECT_EQ(m_cpu->sp(), sp_before - 8); // no extra sub
}

TEST_F(IndirectCtrlTest, Enter_NestingLevel1) {
    // C8 00 00 01 — ENTER 0, 1: Push RBP; FrameTemp=RSP; Push FrameTemp; RBP=FrameTemp
    u64 old_rbp = 0xAAAABBBBCCCCDDDDull;
    m_cpu->gpr.rbp = old_rbp;

    u8 code[] = { 0xC8, 0x00, 0x00, 0x01 };
    write_code(code, sizeof(code));

    u64 sp_before = m_cpu->sp();
    auto result = execute_one();

    EXPECT_EQ(result, ExecResult::Continue);
    const u64 frame_temp = sp_before - 8;
    EXPECT_EQ(read_stack_u64(frame_temp), old_rbp);
    EXPECT_EQ(read_stack_u64(frame_temp - 8), frame_temp);
    EXPECT_EQ(m_cpu->gpr.rbp, frame_temp);
    EXPECT_EQ(m_cpu->sp(), frame_temp - 8);
}

TEST_F(IndirectCtrlTest, Enter_NestingLevel2CopiesParentDisplay) {
    // Parent ENTER 0,1 then child ENTER 0,2 — child copies one parent display slot.
    m_cpu->gpr.rbp = 0x1111111111111111ull;
    u8 parent[] = { 0xC8, 0x00, 0x00, 0x01 };
    write_code(parent, sizeof(parent));
    ASSERT_EQ(execute_one(), ExecResult::Continue);
    const u64 parent_frame = m_cpu->gpr.rbp;

    m_cpu->rip = CODE_BASE;
    u8 child[] = { 0xC8, 0x00, 0x00, 0x02 };
    write_code(child, sizeof(child));
    const u64 sp_before = m_cpu->sp();
    ASSERT_EQ(execute_one(), ExecResult::Continue);

    const u64 child_frame = sp_before - 8;
    // [child_frame] = parent_frame (saved RBP)
    // [child_frame-8] = [parent_frame-8] display copy (parent's FrameTemp push)
    // [child_frame-16] = child_frame
    EXPECT_EQ(m_cpu->gpr.rbp, child_frame);
    EXPECT_EQ(read_stack_u64(child_frame), parent_frame);
    EXPECT_EQ(read_stack_u64(child_frame - 8), read_stack_u64(parent_frame - 8));
    EXPECT_EQ(read_stack_u64(child_frame - 16), child_frame);
    EXPECT_EQ(m_cpu->sp(), child_frame - 16);
}

// ============================================================================
// Round-trip: CALL indirect + RET
// ============================================================================

TEST_F(IndirectCtrlTest, CallIndirect_RetRoundTrip) {
    // CALL RCX at 0x1000, target function at 0x2000 does RET
    m_cpu->gpr.rcx = 0x2000;

    // FF D1 — CALL RCX
    u8 call_code[] = { 0xFF, 0xD1 };
    write_code(call_code, sizeof(call_code));

    // C3 — RET at 0x2000
    u8 ret_code[] = { 0xC3 };
    write_code_at(0x2000, ret_code, sizeof(ret_code));

    u64 sp_before = m_cpu->sp();

    // Execute CALL RCX
    auto result = execute_one();
    EXPECT_EQ(result, ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, 0x2000u);
    EXPECT_EQ(m_cpu->sp(), sp_before - 8);
    EXPECT_EQ(read_stack_u64(m_cpu->sp()), CODE_BASE + 2); // return addr

    // Execute RET
    result = execute_one();
    EXPECT_EQ(result, ExecResult::Branch);
    EXPECT_EQ(m_cpu->rip, CODE_BASE + 2);
    EXPECT_EQ(m_cpu->sp(), sp_before); // stack restored
}
