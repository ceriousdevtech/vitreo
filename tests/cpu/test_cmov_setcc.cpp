// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// CMOVcc/SETcc instruction tests - CPU-021

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
// CMOVcc Decoder Tests
// ============================================================================

TEST(CmovSetccDecoder, CmovzR32Rm32) {
    // 0F 44 C1 - CMOVZ EAX, ECX (modrm C1 = reg=0/EAX, rm=1/ECX)
    auto inst = decode({0x0F, 0x44, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::CMOVZ_R32_RM32);
    EXPECT_EQ(inst.length, 3);
    EXPECT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 0); // EAX
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].reg, 1); // ECX
}

TEST(CmovSetccDecoder, CmovnzR32Rm32) {
    // 0F 45 D1 - CMOVNZ EDX, ECX (modrm D1 = reg=2/EDX, rm=1/ECX)
    auto inst = decode({0x0F, 0x45, 0xD1});
    EXPECT_EQ(inst.opcode, Opcode::CMOVNZ_R32_RM32);
    EXPECT_EQ(inst.length, 3);
    EXPECT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 2); // EDX
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].reg, 1); // ECX
}

TEST(CmovSetccDecoder, CmovbR32Rm32) {
    // 0F 42 C1 - CMOVB EAX, ECX
    auto inst = decode({0x0F, 0x42, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::CMOVB_R32_RM32);
    EXPECT_EQ(inst.length, 3);
    EXPECT_EQ(inst.operand_count, 2);
}

TEST(CmovSetccDecoder, CmovlR32Rm32) {
    // 0F 4C C1 - CMOVL EAX, ECX
    auto inst = decode({0x0F, 0x4C, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::CMOVL_R32_RM32);
    EXPECT_EQ(inst.length, 3);
}

TEST(CmovSetccDecoder, CmovnleR32Rm32) {
    // 0F 4F C1 - CMOVNLE/CMOVG EAX, ECX
    auto inst = decode({0x0F, 0x4F, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::CMOVNLE_R32_RM32);
    EXPECT_EQ(inst.length, 3);
}

TEST(CmovSetccDecoder, CmovzR64Rm64) {
    // 48 0F 44 C1 - CMOVZ RAX, RCX (REX.W prefix)
    auto inst = decode({0x48, 0x0F, 0x44, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::CMOVZ_R64_RM64);
    EXPECT_EQ(inst.length, 4);
    EXPECT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 0); // RAX
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[1].reg, 1); // RCX
}

// ============================================================================
// SETcc Decoder Tests
// ============================================================================

TEST(CmovSetccDecoder, SetzRm8) {
    // 0F 94 C1 - SETZ CL (modrm C1 = mod=3, rm=1/CL)
    auto inst = decode({0x0F, 0x94, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::SETZ_RM8);
    EXPECT_EQ(inst.length, 3);
    EXPECT_EQ(inst.operand_count, 1);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[0].reg, 1); // CL
}

TEST(CmovSetccDecoder, SetbRm8) {
    // 0F 92 C0 - SETB AL (modrm C0 = mod=3, rm=0/AL)
    auto inst = decode({0x0F, 0x92, 0xC0});
    EXPECT_EQ(inst.opcode, Opcode::SETB_RM8);
    EXPECT_EQ(inst.length, 3);
    EXPECT_EQ(inst.operand_count, 1);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[0].reg, 0); // AL
}

TEST(CmovSetccDecoder, SetlRm8) {
    // 0F 9C C2 - SETL DL (modrm C2 = mod=3, rm=2/DL)
    auto inst = decode({0x0F, 0x9C, 0xC2});
    EXPECT_EQ(inst.opcode, Opcode::SETL_RM8);
    EXPECT_EQ(inst.length, 3);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[0].reg, 2); // DL
}

TEST(CmovSetccDecoder, SetoRm8) {
    // 0F 90 C0 - SETO AL
    auto inst = decode({0x0F, 0x90, 0xC0});
    EXPECT_EQ(inst.opcode, Opcode::SETO_RM8);
}

TEST(CmovSetccDecoder, SetnleRm8) {
    // 0F 9F C3 - SETNLE BL (modrm C3 = mod=3, rm=3/BL)
    auto inst = decode({0x0F, 0x9F, 0xC3});
    EXPECT_EQ(inst.opcode, Opcode::SETNLE_RM8);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[0].reg, 3); // BL
}

// ============================================================================
// Interpreter Test Fixture
// ============================================================================

class CmovSetccTest : public ::testing::Test {
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

    void write_code(const u8* code, usize len) {
        m_memory->write_bytes(0x1000, code, len);
    }

    ExecResult execute_one() {
        Interpreter interp(*m_cpu, *m_memory);
        return interp.step();
    }

    std::unique_ptr<GuestMemory> m_memory;
    std::unique_ptr<CpuState> m_cpu;
};

// ============================================================================
// CMOVcc Interpreter Tests
// ============================================================================

TEST_F(CmovSetccTest, CmovzWithZfSet_Moves) {
    // 0F 44 C1 - CMOVZ EAX, ECX
    u8 code[] = { 0x0F, 0x44, 0xC1 };
    write_code(code, sizeof(code));

    m_cpu->gpr.rax = 0x11111111;
    m_cpu->gpr.rcx = 0x22222222;
    m_cpu->flags.set_zf(true);

    auto result = execute_one();
    EXPECT_EQ(result, ExecResult::Continue);
    // 32-bit write zero-extends to 64-bit
    EXPECT_EQ(m_cpu->gpr.rax, 0x22222222ULL);
}

TEST_F(CmovSetccTest, CmovzWithZfClear_NoMove) {
    // 0F 44 C1 - CMOVZ EAX, ECX
    u8 code[] = { 0x0F, 0x44, 0xC1 };
    write_code(code, sizeof(code));

    m_cpu->gpr.rax = 0x11111111;
    m_cpu->gpr.rcx = 0x22222222;
    m_cpu->flags.set_zf(false);

    auto result = execute_one();
    EXPECT_EQ(result, ExecResult::Continue);
    // Destination unchanged
    EXPECT_EQ(m_cpu->gpr.rax, 0x11111111ULL);
}

TEST_F(CmovSetccTest, CmovnzBasic) {
    // 0F 45 C1 - CMOVNZ EAX, ECX
    u8 code[] = { 0x0F, 0x45, 0xC1 };
    write_code(code, sizeof(code));

    m_cpu->gpr.rax = 0xAAAAAAAA;
    m_cpu->gpr.rcx = 0xBBBBBBBB;
    m_cpu->flags.set_zf(false); // NZ condition met

    auto result = execute_one();
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.rax, 0xBBBBBBBBULL);
}

TEST_F(CmovSetccTest, CmovbWithCfSet_Moves) {
    // 0F 42 C1 - CMOVB EAX, ECX
    u8 code[] = { 0x0F, 0x42, 0xC1 };
    write_code(code, sizeof(code));

    m_cpu->gpr.rax = 0x10;
    m_cpu->gpr.rcx = 0x20;
    m_cpu->flags.set_cf(true); // B condition met

    auto result = execute_one();
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.rax, 0x20ULL);
}

TEST_F(CmovSetccTest, CmovlWithSfNeOf_Moves) {
    // 0F 4C C1 - CMOVL EAX, ECX (SF!=OF → move)
    u8 code[] = { 0x0F, 0x4C, 0xC1 };
    write_code(code, sizeof(code));

    m_cpu->gpr.rax = 0x100;
    m_cpu->gpr.rcx = 0x200;
    m_cpu->flags.set_sf(true);
    m_cpu->flags.set_of(false); // SF != OF → L condition met

    auto result = execute_one();
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.rax, 0x200ULL);
}

TEST_F(CmovSetccTest, CmovgWithZfClearAndSfEqOf) {
    // 0F 4F C1 - CMOVNLE/CMOVG EAX, ECX (ZF=0 && SF==OF → move)
    u8 code[] = { 0x0F, 0x4F, 0xC1 };
    write_code(code, sizeof(code));

    m_cpu->gpr.rax = 0x100;
    m_cpu->gpr.rcx = 0x999;
    m_cpu->flags.set_zf(false);
    m_cpu->flags.set_sf(false);
    m_cpu->flags.set_of(false); // ZF=0 && SF==OF → G condition met

    auto result = execute_one();
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.rax, 0x999ULL);
}

TEST_F(CmovSetccTest, Cmovz64bit) {
    // 48 0F 44 C1 - CMOVZ RAX, RCX (REX.W)
    u8 code[] = { 0x48, 0x0F, 0x44, 0xC1 };
    write_code(code, sizeof(code));

    m_cpu->gpr.rax = 0xDEADDEADDEADDEADULL;
    m_cpu->gpr.rcx = 0x123456789ABCDEF0ULL;
    m_cpu->flags.set_zf(true);

    auto result = execute_one();
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.rax, 0x123456789ABCDEF0ULL);
}

TEST_F(CmovSetccTest, CmovccDoesNotModifyFlags) {
    // 0F 44 C1 - CMOVZ EAX, ECX
    u8 code[] = { 0x0F, 0x44, 0xC1 };
    write_code(code, sizeof(code));

    m_cpu->gpr.rax = 0;
    m_cpu->gpr.rcx = 42;
    m_cpu->flags.set_zf(true);
    m_cpu->flags.set_cf(true);
    m_cpu->flags.set_sf(true);
    m_cpu->flags.set_of(true);
    m_cpu->flags.set_pf(true);

    u64 flags_before = m_cpu->flags.rflags;
    auto result = execute_one();
    EXPECT_EQ(result, ExecResult::Continue);

    // Flags must be unchanged
    EXPECT_EQ(m_cpu->flags.rflags, flags_before);
}

// ============================================================================
// SETcc Interpreter Tests
// ============================================================================

TEST_F(CmovSetccTest, SetzWithZfSet_WritesOne) {
    // 0F 94 C1 - SETZ CL
    u8 code[] = { 0x0F, 0x94, 0xC1 };
    write_code(code, sizeof(code));

    m_cpu->gpr.rcx = 0xFF00FF00FF00FF00ULL; // garbage in upper bits
    m_cpu->flags.set_zf(true);

    auto result = execute_one();
    EXPECT_EQ(result, ExecResult::Continue);
    // CL (low byte of RCX) should be 1, rest of RCX preserved
    EXPECT_EQ(m_cpu->gpr.rcx & 0xFF, 1ULL);
}

TEST_F(CmovSetccTest, SetzWithZfClear_WritesZero) {
    // 0F 94 C1 - SETZ CL
    u8 code[] = { 0x0F, 0x94, 0xC1 };
    write_code(code, sizeof(code));

    m_cpu->gpr.rcx = 0xFF;
    m_cpu->flags.set_zf(false);

    auto result = execute_one();
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.rcx & 0xFF, 0ULL);
}

TEST_F(CmovSetccTest, SetbWithCfSet_WritesOne) {
    // 0F 92 C0 - SETB AL
    u8 code[] = { 0x0F, 0x92, 0xC0 };
    write_code(code, sizeof(code));

    m_cpu->gpr.rax = 0;
    m_cpu->flags.set_cf(true);

    auto result = execute_one();
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.rax & 0xFF, 1ULL);
}

TEST_F(CmovSetccTest, SetlBothConditions) {
    // 0F 9C C2 - SETL DL (SF != OF)
    u8 code[] = { 0x0F, 0x9C, 0xC2 };

    // Test 1: SF=1, OF=0 → SF!=OF → 1
    write_code(code, sizeof(code));
    m_cpu->gpr.rdx = 0;
    m_cpu->flags.set_sf(true);
    m_cpu->flags.set_of(false);
    m_cpu->rip = 0x1000;

    auto result = execute_one();
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.rdx & 0xFF, 1ULL);

    // Test 2: SF=0, OF=0 → SF==OF → 0
    write_code(code, sizeof(code));
    m_cpu->gpr.rdx = 0xFF;
    m_cpu->flags.set_sf(false);
    m_cpu->flags.set_of(false);
    m_cpu->rip = 0x1000;

    result = execute_one();
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.rdx & 0xFF, 0ULL);
}

TEST_F(CmovSetccTest, SetccToMemoryOperand) {
    // 0F 94 05 F9 1F 00 00 - SETZ [RIP+0x1FF9]
    // modrm 05 = mod=0, reg=0, rm=5 → RIP-relative in 64-bit mode
    // RIP-relative: EA = next_rip + disp32 (next_rip = 0x1000 + 7 = 0x1007)
    // Want target at 0x3000: disp = 0x3000 - 0x1007 = 0x1FF9
    u8 code[] = { 0x0F, 0x94, 0x05, 0xF9, 0x1F, 0x00, 0x00 };
    write_code(code, sizeof(code));

    m_cpu->flags.set_zf(true);

    // Effective address = 0x1007 + 0x1FF9 = 0x3000
    // Zero the target first
    m_memory->write_u8(0x3000, 0x00);

    auto result = execute_one();
    EXPECT_EQ(result, ExecResult::Continue);

    auto val = m_memory->read_u8(0x3000);
    ASSERT_TRUE(val.is_ok());
    EXPECT_EQ(val.value(), 1);
}

// ============================================================================
// Lazy-flag hand-off from the JIT
//
// SETcc and CMOVcc are pure flag consumers, so they must settle a lazy flag op
// left pending by a compiled block instead of reading the stale rflags image.
// ============================================================================

namespace {

/// Record the lazy state the JIT would leave behind for `CMP a, b`, and poison
/// the architectural EFLAGS so a stale read is unambiguously visible.
void stage_pending_cmp32(CpuState& cpu, u32 a, u32 b) {
    cpu.flags.rflags = 0;
    cpu.flags.set_zf(a != b);   // deliberately inverted vs. the real result
    cpu.flag_op = CpuState::FlagOp::Sub32;
    cpu.flag_src = b;
    cpu.flag_dst = static_cast<u32>(a - b);
}

} // namespace

TEST_F(CmovSetccTest, SetzObservesPendingLazyCompare) {
    // 0F 94 C0 - SETZ AL. Pending CMP 5,3 is not equal, so AL must be 0 even
    // though the stale EFLAGS image has ZF=1.
    u8 code[] = { 0x0F, 0x94, 0xC0 };
    write_code(code, sizeof(code));

    m_cpu->gpr.rax = 0xFFFFFFFFFFFFFFFFULL;
    stage_pending_cmp32(*m_cpu, 5, 3);

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.rax & 0xFF, 0u);
    EXPECT_EQ(m_cpu->flag_op, CpuState::FlagOp::None);
}

TEST_F(CmovSetccTest, SetzObservesPendingLazyEqualCompare) {
    // 0F 94 C0 - SETZ AL. Pending CMP 7,7 is equal, so AL must be 1 even
    // though the stale EFLAGS image has ZF=0.
    u8 code[] = { 0x0F, 0x94, 0xC0 };
    write_code(code, sizeof(code));

    m_cpu->gpr.rax = 0;
    stage_pending_cmp32(*m_cpu, 7, 7);

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.rax & 0xFF, 1u);
}

TEST_F(CmovSetccTest, CmovzObservesPendingLazyCompare) {
    // 0F 44 C1 - CMOVZ EAX, ECX. Pending CMP 5,3 is not equal, so no move
    // happens even though the stale EFLAGS image has ZF=1.
    u8 code[] = { 0x0F, 0x44, 0xC1 };
    write_code(code, sizeof(code));

    m_cpu->gpr.rax = 0x11111111;
    m_cpu->gpr.rcx = 0x22222222;
    stage_pending_cmp32(*m_cpu, 5, 3);

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.rax, 0x11111111ULL);
}

