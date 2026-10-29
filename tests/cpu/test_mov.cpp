// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// MOV instruction comprehensive tests

#include <gtest/gtest.h>
#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"

using namespace vitreo;
using namespace vitreo::cpu;

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

TEST(MovDecode, XlatDecodesWithAddressAndSegmentOverrides) {
    auto plain = decode({0xD7}, DecodeMode::Mode32);
    EXPECT_EQ(plain.opcode, Opcode::XLAT);
    EXPECT_EQ(plain.length, 1u);
    EXPECT_EQ(plain.operand_count, 0u);

    auto overridden = decode({0x64, 0x67, 0xD7}, DecodeMode::Mode64);
    EXPECT_EQ(overridden.opcode, Opcode::XLAT);
    EXPECT_EQ(overridden.length, 3u);
    EXPECT_TRUE(overridden.prefixes.address_size);
    EXPECT_EQ(overridden.prefixes.segment, SegReg::FS);

    expect_decode_fail({0xF0, 0xD7}, DecodeMode::Mode32);
}

// ----------------------------------------------------------------------------
// MOV r8, imm8 (B0-B7)
// ----------------------------------------------------------------------------

TEST(MovDecode, MovAlImm8) {
    // B0 42 - MOV AL, 0x42
    auto inst = decode({0xB0, 0x42});
    EXPECT_EQ(inst.opcode, Opcode::MOV_R8_IMM8);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[0].reg, 0);  // AL
    EXPECT_EQ(inst.operands[1].type, OperandType::Imm8);
    EXPECT_EQ(inst.operands[1].imm, 0x42);
}

TEST(MovDecode, MovClImm8) {
    // B1 FF - MOV CL, 0xFF
    auto inst = decode({0xB1, 0xFF});
    EXPECT_EQ(inst.opcode, Opcode::MOV_R8_IMM8);
    EXPECT_EQ(inst.operands[0].reg, 1);  // CL
    EXPECT_EQ(inst.operands[1].imm, 0xFF);
}

TEST(MovDecode, MovR8Imm8WithRex) {
    // 41 B0 42 - MOV R8B, 0x42
    auto inst = decode({0x41, 0xB0, 0x42});
    EXPECT_EQ(inst.opcode, Opcode::MOV_R8_IMM8);
    EXPECT_EQ(inst.operands[0].reg, 8);  // R8B (REX.B extends)
}

// ----------------------------------------------------------------------------
// MOV r16/32/64, imm16/32/64 (B8-BF)
// ----------------------------------------------------------------------------

TEST(MovDecode, MovEaxImm32) {
    // B8 78 56 34 12 - MOV EAX, 0x12345678
    auto inst = decode({0xB8, 0x78, 0x56, 0x34, 0x12});
    EXPECT_EQ(inst.opcode, Opcode::MOV_R32_IMM32);
    EXPECT_EQ(inst.length, 5);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 0);  // EAX
    EXPECT_EQ(inst.operands[1].type, OperandType::Imm32);
    EXPECT_EQ(inst.operands[1].imm, 0x12345678);
}

TEST(MovDecode, MovRaxImm64) {
    // 48 B8 <8 bytes> - MOV RAX, imm64
    auto inst = decode({0x48, 0xB8, 0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01});
    EXPECT_EQ(inst.opcode, Opcode::MOV_R64_IMM64);
    EXPECT_EQ(inst.length, 10);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 0);  // RAX
    EXPECT_EQ(inst.operands[1].type, OperandType::Imm64);
    EXPECT_EQ(static_cast<u64>(inst.operands[1].imm), 0x0123456789ABCDEFULL);
}

TEST(MovDecode, MovR12Imm64) {
    // 49 BC <8 bytes> - MOV R12, imm64
    auto inst = decode({0x49, 0xBC, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08});
    EXPECT_EQ(inst.opcode, Opcode::MOV_R64_IMM64);
    EXPECT_EQ(inst.operands[0].reg, 12);  // R12 (4 + REX.B*8)
}

TEST(MovDecode, MovAxImm16) {
    // 66 B8 34 12 - MOV AX, 0x1234
    auto inst = decode({0x66, 0xB8, 0x34, 0x12});
    EXPECT_EQ(inst.opcode, Opcode::MOV_R16_IMM16);
    EXPECT_EQ(inst.length, 4);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg16);
    EXPECT_EQ(inst.operands[1].imm, 0x1234);
}

// ----------------------------------------------------------------------------
// MOV r/m, r (88, 89) and MOV r, r/m (8A, 8B)
// ----------------------------------------------------------------------------

TEST(MovDecode, MovRm8R8) {
    // 88 D8 - MOV AL, BL (mod=3)
    auto inst = decode({0x88, 0xD8});
    EXPECT_EQ(inst.opcode, Opcode::MOV_RM8_R8);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[0].reg, 0);  // AL
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[1].reg, 3);  // BL
}

TEST(MovDecode, MovMemR8) {
    // 88 03 - MOV [RBX], AL
    auto inst = decode({0x88, 0x03});
    EXPECT_EQ(inst.opcode, Opcode::MOV_RM8_R8);
    EXPECT_EQ(inst.operands[0].type, OperandType::Mem8);
    EXPECT_EQ(inst.operands[0].mem.base, 3);  // RBX
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[1].reg, 0);  // AL
}

TEST(MovDecode, MovRm32R32) {
    // 89 D8 - MOV EAX, EBX (mod=3)
    auto inst = decode({0x89, 0xD8});
    EXPECT_EQ(inst.opcode, Opcode::MOV_RM32_R32);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg32);
}

TEST(MovDecode, MovRm64R64) {
    // 48 89 D8 - MOV RAX, RBX
    auto inst = decode({0x48, 0x89, 0xD8});
    EXPECT_EQ(inst.opcode, Opcode::MOV_RM64_R64);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg64);
}

TEST(MovDecode, MovMemR64WithDisp) {
    // 48 89 43 10 - MOV [RBX+0x10], RAX
    auto inst = decode({0x48, 0x89, 0x43, 0x10});
    EXPECT_EQ(inst.opcode, Opcode::MOV_RM64_R64);
    EXPECT_EQ(inst.operands[0].type, OperandType::Mem64);
    EXPECT_EQ(inst.operands[0].mem.base, 3);   // RBX
    EXPECT_EQ(inst.operands[0].mem.disp, 0x10);
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[1].reg, 0);  // RAX
}

TEST(MovDecode, MovMemRspDisp32ConsumesFullInstruction) {
    // 48 89 84 24 E0 02 00 00 - MOV [RSP+0x2E0], RAX
    auto inst = decode({0x48, 0x89, 0x84, 0x24, 0xE0, 0x02, 0x00, 0x00});
    EXPECT_EQ(inst.opcode, Opcode::MOV_RM64_R64);
    EXPECT_EQ(inst.length, 8);
    ASSERT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Mem64);
    EXPECT_EQ(inst.operands[0].mem.base, 4);  // RSP
    EXPECT_EQ(inst.operands[0].mem.index, 255);
    EXPECT_EQ(inst.operands[0].mem.disp, 0x2E0);
    EXPECT_EQ(inst.operands[1].reg, 0);  // RAX
}

TEST(MovDecode, MovR8Rm8) {
    // 8A C3 - MOV AL, BL
    auto inst = decode({0x8A, 0xC3});
    EXPECT_EQ(inst.opcode, Opcode::MOV_R8_RM8);
    EXPECT_EQ(inst.operands[0].reg, 0);  // AL
    EXPECT_EQ(inst.operands[1].reg, 3);  // BL
}

TEST(MovDecode, MovR32Rm32) {
    // 8B C3 - MOV EAX, EBX
    auto inst = decode({0x8B, 0xC3});
    EXPECT_EQ(inst.opcode, Opcode::MOV_R32_RM32);
}

TEST(MovDecode, MovR64Rm64) {
    // 48 8B C3 - MOV RAX, RBX
    auto inst = decode({0x48, 0x8B, 0xC3});
    EXPECT_EQ(inst.opcode, Opcode::MOV_R64_RM64);
}

TEST(MovDecode, MovR64MemWithSIB) {
    // 48 8B 04 88 - MOV RAX, [RAX + RCX*4]
    auto inst = decode({0x48, 0x8B, 0x04, 0x88});
    EXPECT_EQ(inst.opcode, Opcode::MOV_R64_RM64);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 0);  // RAX
    EXPECT_EQ(inst.operands[1].type, OperandType::Mem64);
    EXPECT_EQ(inst.operands[1].mem.base, 0);   // RAX
    EXPECT_EQ(inst.operands[1].mem.index, 1);  // RCX
    EXPECT_EQ(inst.operands[1].mem.scale, 4);
}

TEST(MovDecode, MovRIPRelative) {
    // 48 8B 05 00 10 00 00 - MOV RAX, [RIP+0x1000]
    auto inst = decode({0x48, 0x8B, 0x05, 0x00, 0x10, 0x00, 0x00});
    EXPECT_EQ(inst.opcode, Opcode::MOV_R64_RM64);
    EXPECT_EQ(inst.operands[1].type, OperandType::Mem64);
    EXPECT_TRUE(inst.operands[1].mem.is_rip_relative());
    EXPECT_EQ(inst.operands[1].mem.disp, 0x1000);
}

// ----------------------------------------------------------------------------
// MOV r/m, imm (C6, C7)
// ----------------------------------------------------------------------------

TEST(MovDecode, MovRm8Imm8) {
    // C6 C0 42 - MOV AL, 0x42 (mod=3, rm=0)
    auto inst = decode({0xC6, 0xC0, 0x42});
    EXPECT_EQ(inst.opcode, Opcode::MOV_RM8_IMM8);
    EXPECT_EQ(inst.length, 3);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[0].reg, 0);  // AL
    EXPECT_EQ(inst.operands[1].type, OperandType::Imm8);
    EXPECT_EQ(inst.operands[1].imm, 0x42);
}

TEST(MovDecode, MovMemImm8) {
    // C6 03 42 - MOV BYTE PTR [RBX], 0x42
    auto inst = decode({0xC6, 0x03, 0x42});
    EXPECT_EQ(inst.opcode, Opcode::MOV_RM8_IMM8);
    EXPECT_EQ(inst.operands[0].type, OperandType::Mem8);
    EXPECT_EQ(inst.operands[0].mem.base, 3);  // RBX
    EXPECT_EQ(inst.operands[1].imm, 0x42);
}

TEST(MovDecode, MovRm32Imm32) {
    // C7 C0 78 56 34 12 - MOV EAX, 0x12345678
    auto inst = decode({0xC7, 0xC0, 0x78, 0x56, 0x34, 0x12});
    EXPECT_EQ(inst.opcode, Opcode::MOV_RM32_IMM32);
    EXPECT_EQ(inst.length, 6);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].imm, 0x12345678);
}

TEST(MovDecode, MovMemImm32) {
    // C7 03 78 56 34 12 - MOV DWORD PTR [RBX], 0x12345678
    auto inst = decode({0xC7, 0x03, 0x78, 0x56, 0x34, 0x12});
    EXPECT_EQ(inst.opcode, Opcode::MOV_RM32_IMM32);
    EXPECT_EQ(inst.operands[0].type, OperandType::Mem32);
    EXPECT_EQ(inst.operands[0].mem.base, 3);  // RBX
    EXPECT_EQ(inst.operands[1].imm, 0x12345678);
}

TEST(MovDecode, MovRm64Imm32SignExtended) {
    // 48 C7 C0 FF FF FF FF - MOV RAX, -1 (sign-extended imm32)
    auto inst = decode({0x48, 0xC7, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF});
    EXPECT_EQ(inst.opcode, Opcode::MOV_RM64_IMM32);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 0);  // RAX
    EXPECT_EQ(inst.operands[1].type, OperandType::Imm32);
    EXPECT_EQ(static_cast<i32>(inst.operands[1].imm), -1);
}

TEST(MovDecode, MovMem64Imm32) {
    // 48 C7 03 78 56 34 12 - MOV QWORD PTR [RBX], 0x12345678
    auto inst = decode({0x48, 0xC7, 0x03, 0x78, 0x56, 0x34, 0x12});
    EXPECT_EQ(inst.opcode, Opcode::MOV_RM64_IMM32);
    EXPECT_EQ(inst.operands[0].type, OperandType::Mem64);
    EXPECT_EQ(inst.operands[1].imm, 0x12345678);
}

TEST(MovDecode, MovRm16Imm16) {
    // 66 C7 C0 34 12 - MOV AX, 0x1234
    auto inst = decode({0x66, 0xC7, 0xC0, 0x34, 0x12});
    EXPECT_EQ(inst.opcode, Opcode::MOV_RM16_IMM16);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg16);
    EXPECT_EQ(inst.operands[1].imm, 0x1234);
}

// ----------------------------------------------------------------------------
// MOV with moffs (A0-A3)
// ----------------------------------------------------------------------------

TEST(MovDecode, MovAlMoffs8_32bit) {
    // A0 78 56 34 12 - MOV AL, [0x12345678] (32-bit mode)
    auto inst = decode({0xA0, 0x78, 0x56, 0x34, 0x12}, DecodeMode::Mode32);
    EXPECT_EQ(inst.opcode, Opcode::MOV_AL_MOFFS8);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[0].reg, 0);  // AL
    EXPECT_EQ(inst.operands[1].type, OperandType::Mem8);
    EXPECT_EQ(inst.operands[1].mem.disp, 0x12345678);
}

TEST(MovDecode, MovEaxMoffs32_32bit) {
    // A1 78 56 34 12 - MOV EAX, [0x12345678] (32-bit mode)
    auto inst = decode({0xA1, 0x78, 0x56, 0x34, 0x12}, DecodeMode::Mode32);
    EXPECT_EQ(inst.opcode, Opcode::MOV_EAX_MOFFS32);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].type, OperandType::Mem32);
}

TEST(MovDecode, MovMoffs8Al_32bit) {
    // A2 78 56 34 12 - MOV [0x12345678], AL (32-bit mode)
    auto inst = decode({0xA2, 0x78, 0x56, 0x34, 0x12}, DecodeMode::Mode32);
    EXPECT_EQ(inst.opcode, Opcode::MOV_MOFFS8_AL);
    EXPECT_EQ(inst.operands[0].type, OperandType::Mem8);
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[1].reg, 0);  // AL
}

TEST(MovDecode, MovMoffs32Eax_32bit) {
    // A3 78 56 34 12 - MOV [0x12345678], EAX (32-bit mode)
    auto inst = decode({0xA3, 0x78, 0x56, 0x34, 0x12}, DecodeMode::Mode32);
    EXPECT_EQ(inst.opcode, Opcode::MOV_MOFFS32_EAX);
    EXPECT_EQ(inst.operands[0].type, OperandType::Mem32);
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg32);
}

TEST(MovDecode, MovRaxMoffs64_64bit) {
    // 48 A1 <8 bytes> - MOV RAX, [moffs64]
    auto inst = decode({0x48, 0xA1, 0x20, 0x45, 0x14, 0x00,
                        0xD4, 0x37, 0x00, 0x00});
    EXPECT_EQ(inst.opcode, Opcode::MOV_RAX_MOFFS64);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[1].type, OperandType::Mem64);
    EXPECT_TRUE(inst.operands[1].mem.has_absolute);
    EXPECT_EQ(inst.operands[1].mem.absolute, 0x000037D400144520ULL);
}

// ============================================================================
// Interpreter Tests
// ============================================================================

// Convenience register indices
constexpr u8 RAX = static_cast<u8>(RegIndex::RAX);
constexpr u8 RCX = static_cast<u8>(RegIndex::RCX);
constexpr u8 RDX = static_cast<u8>(RegIndex::RDX);
constexpr u8 RBX = static_cast<u8>(RegIndex::RBX);
constexpr u8 RSP = static_cast<u8>(RegIndex::RSP);

class MovInterpreterTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        auto map_result = m_memory->map_region(0x1000, 0x10000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(map_result);
        
        m_cpu = std::make_unique<CpuState>();
        m_cpu->init_long_mode();
        m_cpu->gpr.set64(RSP, 0x10000);
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

// ----------------------------------------------------------------------------
// MOV r, imm tests
// ----------------------------------------------------------------------------

TEST_F(MovInterpreterTest, MovR8Imm8) {
    // MOV AL, 0x42
    u8 code[] = { 0xB0, 0x42 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get8l(0), 0x42);
    EXPECT_EQ(m_cpu->rip, 0x1002);
}

TEST_F(MovInterpreterTest, MovR32Imm32_ClearsUpperBits) {
    // Set upper bits first
    m_cpu->gpr.set64(RAX, 0xFFFFFFFF00000000ULL);
    
    // MOV EAX, 0x12345678
    u8 code[] = { 0xB8, 0x78, 0x56, 0x34, 0x12 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // Upper 32 bits should be cleared (x86-64 behavior)
    EXPECT_EQ(m_cpu->gpr.get64(RAX), 0x12345678ULL);
}

TEST_F(MovInterpreterTest, MovR64Imm64) {
    // MOV RAX, 0x123456789ABCDEF0
    u8 code[] = { 0x48, 0xB8, 0xF0, 0xDE, 0xBC, 0x9A, 0x78, 0x56, 0x34, 0x12 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(RAX), 0x123456789ABCDEF0ULL);
}

TEST_F(MovInterpreterTest, MovRaxMoffs64UsesFullAbsoluteAddress) {
    constexpr u64 address = 0x000037D400144520ULL;
    constexpr u64 page = address & ~0xFFFULL;
    constexpr u64 value = 0x0123456789ABCDEFULL;
    ASSERT_TRUE(m_memory->map_region(page, 0x1000,
                                     MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->write_u64(address, value));

    // Intel SDM Vol. 2 MOV: REX.W + A1 encodes an eight-byte moffs64.
    const u8 code[] = {0x48, 0xA1, 0x20, 0x45, 0x14, 0x00,
                       0xD4, 0x37, 0x00, 0x00};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(RAX), value);
    EXPECT_EQ(m_cpu->rip, 0x1000 + sizeof(code));
}

TEST_F(MovInterpreterTest, MovR16Imm16_PreservesUpper) {
    m_cpu->gpr.set64(RAX, 0x123456780000FFFFULL);
    
    // MOV AX, 0x1234 (66 B8 34 12)
    u8 code[] = { 0x66, 0xB8, 0x34, 0x12 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // Only lower 16 bits should change
    EXPECT_EQ(m_cpu->gpr.get64(RAX), 0x1234567800001234ULL);
}

// ----------------------------------------------------------------------------
// MOV r, r tests
// ----------------------------------------------------------------------------

TEST_F(MovInterpreterTest, MovR64R64) {
    m_cpu->gpr.set64(RBX, 0xDEADBEEFCAFEBABEULL);
    
    // MOV RAX, RBX (48 8B C3)
    u8 code[] = { 0x48, 0x8B, 0xC3 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(RAX), 0xDEADBEEFCAFEBABEULL);
}

TEST_F(MovInterpreterTest, MovR32R32_ClearsUpper) {
    m_cpu->gpr.set64(RAX, 0xFFFFFFFFFFFFFFFFULL);
    m_cpu->gpr.set64(RBX, 0x0000000012345678ULL);
    
    // MOV EAX, EBX (8B C3)
    u8 code[] = { 0x8B, 0xC3 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // Upper bits cleared when writing to 32-bit reg
    EXPECT_EQ(m_cpu->gpr.get64(RAX), 0x12345678ULL);
}

// ----------------------------------------------------------------------------
// MOV r, [mem] tests
// ----------------------------------------------------------------------------

TEST_F(MovInterpreterTest, MovR64Mem) {
    // Write test value to memory
    m_cpu->gpr.set64(RBX, 0x2000);
    u64 test_val = 0xCAFEBABEDEADBEEFULL;
    m_memory->write_u64(0x2000, test_val);
    
    // MOV RAX, [RBX] (48 8B 03)
    u8 code[] = { 0x48, 0x8B, 0x03 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(RAX), test_val);
}

TEST_F(MovInterpreterTest, MovR32MemWithDisp) {
    m_cpu->gpr.set64(RBX, 0x2000);
    m_memory->write_u32(0x2010, 0x12345678);
    
    // MOV EAX, [RBX+0x10] (8B 43 10)
    u8 code[] = { 0x8B, 0x43, 0x10 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(RAX), 0x12345678U);
}

TEST_F(MovInterpreterTest, MovR64MemWithSIB) {
    m_cpu->gpr.set64(RAX, 0x2000);
    m_cpu->gpr.set64(RCX, 0x10);
    m_memory->write_u64(0x2040, 0xAAAABBBBCCCCDDDDULL);
    
    // MOV RDX, [RAX + RCX*4] (48 8B 14 88)
    u8 code[] = { 0x48, 0x8B, 0x14, 0x88 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(RDX), 0xAAAABBBBCCCCDDDDULL);
}

// ----------------------------------------------------------------------------
// MOV [mem], r tests
// ----------------------------------------------------------------------------

TEST_F(MovInterpreterTest, MovMemR64) {
    m_cpu->gpr.set64(RAX, 0xFEEDFACEDECAF000ULL);
    m_cpu->gpr.set64(RBX, 0x2000);
    
    // MOV [RBX], RAX (48 89 03)
    u8 code[] = { 0x48, 0x89, 0x03 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    auto result = m_memory->read_u64(0x2000);
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), 0xFEEDFACEDECAF000ULL);
}

TEST_F(MovInterpreterTest, MovMemR8) {
    m_cpu->gpr.set8l(0, 0x42);  // AL
    m_cpu->gpr.set64(RBX, 0x2000);
    
    // MOV [RBX], AL (88 03)
    u8 code[] = { 0x88, 0x03 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    auto result = m_memory->read_u8(0x2000);
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), 0x42);
}

// ----------------------------------------------------------------------------
// MOV [mem], imm tests (C6, C7)
// ----------------------------------------------------------------------------

TEST_F(MovInterpreterTest, MovMemImm8) {
    m_cpu->gpr.set64(RBX, 0x2000);
    
    // MOV BYTE PTR [RBX], 0x42 (C6 03 42)
    u8 code[] = { 0xC6, 0x03, 0x42 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    auto result = m_memory->read_u8(0x2000);
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), 0x42);
}

TEST_F(MovInterpreterTest, MovMemImm32) {
    m_cpu->gpr.set64(RBX, 0x2000);
    
    // MOV DWORD PTR [RBX], 0x12345678 (C7 03 78 56 34 12)
    u8 code[] = { 0xC7, 0x03, 0x78, 0x56, 0x34, 0x12 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    auto result = m_memory->read_u32(0x2000);
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), 0x12345678U);
}

TEST_F(MovInterpreterTest, MovRegImm32ViaC7) {
    // MOV EAX, 0x12345678 (C7 C0 78 56 34 12)
    u8 code[] = { 0xC7, 0xC0, 0x78, 0x56, 0x34, 0x12 };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(RAX), 0x12345678U);
}

TEST_F(MovInterpreterTest, MovMem64Imm32SignExtended) {
    m_cpu->gpr.set64(RBX, 0x2000);
    
    // MOV QWORD PTR [RBX], 0xFFFFFFFF (-1) (48 C7 03 FF FF FF FF)
    u8 code[] = { 0x48, 0xC7, 0x03, 0xFF, 0xFF, 0xFF, 0xFF };
    write_code(code, sizeof(code));
    
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    auto result = m_memory->read_u64(0x2000);
    ASSERT_TRUE(result);
    // Should be sign-extended to 0xFFFFFFFFFFFFFFFF
    EXPECT_EQ(result.value(), 0xFFFFFFFFFFFFFFFFULL);
}

// ----------------------------------------------------------------------------
// Multiple MOV sequence test
// ----------------------------------------------------------------------------

TEST_F(MovInterpreterTest, MovSequence) {
    // MOV RAX, 0x100
    // MOV RBX, RAX
    // MOV [RBX+0x2000], RAX
    u8 code[] = {
        0x48, 0xC7, 0xC0, 0x00, 0x01, 0x00, 0x00,  // MOV RAX, 0x100
        0x48, 0x8B, 0xD8,                          // MOV RBX, RAX
    };
    write_code(code, sizeof(code));
    
    Interpreter interp(*m_cpu, *m_memory);
    EXPECT_EQ(interp.run(2), ExecResult::Continue);
    
    EXPECT_EQ(m_cpu->gpr.get64(RAX), 0x100ULL);
    EXPECT_EQ(m_cpu->gpr.get64(RBX), 0x100ULL);
    EXPECT_EQ(m_cpu->rip, 0x100A);  // After 10 bytes
}
