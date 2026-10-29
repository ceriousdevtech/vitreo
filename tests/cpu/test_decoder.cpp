// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#include "core/cpu/decode/decoder.hpp"
#include <gtest/gtest.h>
#include <vector>

using namespace vitreo;
using namespace vitreo::cpu;

// Helper to decode instruction from bytes
DecodedInstruction decode(const std::initializer_list<u8>& bytes, DecodeMode mode = DecodeMode::Mode64) {
    std::vector<u8> data(bytes);
    MemoryByteReader reader(data.data(), data.size());
    DecodedInstruction inst;
    DecodeError err = decode_instruction(reader, mode, inst);
    EXPECT_EQ(err, DecodeError::Success) << "Decode failed for opcode";
    return inst;
}

// ============================================================================
// NOP Tests
// ============================================================================

TEST(Decoder, Nop) {
    auto inst = decode({0x90});
    EXPECT_EQ(inst.opcode, Opcode::NOP);
    EXPECT_EQ(inst.length, 1);
    EXPECT_EQ(inst.operand_count, 0);
}

TEST(Decoder, Pause) {
    // F3 90 - PAUSE (Intel SDM PAUSE); not NOP
    auto inst = decode({0xF3, 0x90});
    EXPECT_EQ(inst.opcode, Opcode::PAUSE);
    EXPECT_EQ(inst.length, 2);
}

TEST(Decoder, MultiByteNop) {
    // 0F 1F 00 - NOP [rax]
    auto inst = decode({0x0F, 0x1F, 0x00});
    EXPECT_EQ(inst.opcode, Opcode::NOP_RM);
    EXPECT_EQ(inst.length, 3);
}

TEST(Decoder, MemoryFences) {
    auto lfence = decode({0x0F, 0xAE, 0xE8});
    EXPECT_EQ(lfence.opcode, Opcode::LFENCE);
    EXPECT_EQ(lfence.operand_count, 0);

    auto mfence = decode({0x0F, 0xAE, 0xF0});
    EXPECT_EQ(mfence.opcode, Opcode::MFENCE);
    EXPECT_EQ(mfence.operand_count, 0);

    auto sfence = decode({0x0F, 0xAE, 0xF8});
    EXPECT_EQ(sfence.opcode, Opcode::SFENCE);
    EXPECT_EQ(sfence.operand_count, 0);
}

TEST(Decoder, RejectsNonArchitecturalFenceRegisterEncoding) {
    const std::vector<u8> data{0x0F, 0xAE, 0xE9};
    MemoryByteReader reader(data.data(), data.size());
    DecodedInstruction inst;
    EXPECT_EQ(decode_instruction(reader, DecodeMode::Mode64, inst),
              DecodeError::InvalidOperand);
}

// ============================================================================
// MOV Tests
// ============================================================================

TEST(Decoder, MovR8Imm8) {
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

TEST(Decoder, MovR32Imm32) {
    // B8 78 56 34 12 - MOV EAX, 0x12345678
    auto inst = decode({0xB8, 0x78, 0x56, 0x34, 0x12});
    EXPECT_EQ(inst.opcode, Opcode::MOV_R32_IMM32);
    EXPECT_EQ(inst.length, 5);
    EXPECT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 0);  // EAX
    EXPECT_EQ(inst.operands[1].type, OperandType::Imm32);
    EXPECT_EQ(inst.operands[1].imm, 0x12345678);
}

TEST(Decoder, MovR64Imm64) {
    // 48 B8 <8 bytes> - MOV RAX, imm64
    auto inst = decode({0x48, 0xB8, 0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01});
    EXPECT_EQ(inst.opcode, Opcode::MOV_R64_IMM64);
    EXPECT_EQ(inst.length, 10);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 0);  // RAX
    EXPECT_EQ(inst.operands[1].type, OperandType::Imm64);
    EXPECT_EQ(static_cast<u64>(inst.operands[1].imm), 0x0123456789ABCDEFULL);
}

TEST(Decoder, MovRM32R32) {
    // 89 D8 - MOV EAX, EBX (mod=3 rm=0 reg=3)
    auto inst = decode({0x89, 0xD8});
    EXPECT_EQ(inst.opcode, Opcode::MOV_RM32_R32);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 0);  // EAX
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].reg, 3);  // EBX
}

TEST(Decoder, MovR64RM64) {
    // 48 8B 03 - MOV RAX, [RBX]
    auto inst = decode({0x48, 0x8B, 0x03});
    EXPECT_EQ(inst.opcode, Opcode::MOV_R64_RM64);
    EXPECT_EQ(inst.length, 3);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 0);  // RAX
    EXPECT_EQ(inst.operands[1].type, OperandType::Mem64);
    EXPECT_EQ(inst.operands[1].mem.base, 3);  // RBX
}

TEST(Decoder, MovR32RM32WithDisp8) {
    // 8B 43 10 - MOV EAX, [RBX+0x10]
    auto inst = decode({0x8B, 0x43, 0x10});
    EXPECT_EQ(inst.opcode, Opcode::MOV_R32_RM32);
    EXPECT_EQ(inst.length, 3);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 0);  // EAX
    EXPECT_EQ(inst.operands[1].type, OperandType::Mem32);
    EXPECT_EQ(inst.operands[1].mem.base, 3);  // RBX
    EXPECT_EQ(inst.operands[1].mem.disp, 0x10);
}

TEST(Decoder, MovR32RM32WithSIB) {
    // 8B 04 88 - MOV EAX, [RAX + RCX*4]
    auto inst = decode({0x8B, 0x04, 0x88});
    EXPECT_EQ(inst.opcode, Opcode::MOV_R32_RM32);
    EXPECT_EQ(inst.length, 3);
    EXPECT_EQ(inst.operands[1].type, OperandType::Mem32);
    EXPECT_EQ(inst.operands[1].mem.base, 0);   // RAX
    EXPECT_EQ(inst.operands[1].mem.index, 1);  // RCX
    EXPECT_EQ(inst.operands[1].mem.scale, 4);
}

TEST(Decoder, MovRIPRelative) {
    // 8B 05 00 10 00 00 - MOV EAX, [RIP+0x1000]
    auto inst = decode({0x8B, 0x05, 0x00, 0x10, 0x00, 0x00});
    EXPECT_EQ(inst.opcode, Opcode::MOV_R32_RM32);
    EXPECT_EQ(inst.length, 6);
    EXPECT_EQ(inst.operands[1].type, OperandType::Mem32);
    EXPECT_TRUE(inst.operands[1].mem.is_rip_relative());
    EXPECT_EQ(inst.operands[1].mem.disp, 0x1000);
}

// ============================================================================
// ADD Tests
// ============================================================================

TEST(Decoder, AddAlImm8) {
    // 04 10 - ADD AL, 0x10
    auto inst = decode({0x04, 0x10});
    EXPECT_EQ(inst.opcode, Opcode::ADD_AL_IMM8);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operands[1].imm, 0x10);
}

TEST(Decoder, AddEaxImm32) {
    // 05 78 56 34 12 - ADD EAX, 0x12345678
    auto inst = decode({0x05, 0x78, 0x56, 0x34, 0x12});
    EXPECT_EQ(inst.opcode, Opcode::ADD_EAX_IMM32);
    EXPECT_EQ(inst.length, 5);
}

TEST(Decoder, AddRM64R64) {
    // 48 01 D8 - ADD RAX, RBX
    auto inst = decode({0x48, 0x01, 0xD8});
    EXPECT_EQ(inst.opcode, Opcode::ADD_RM64_R64);
    EXPECT_EQ(inst.length, 3);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 0);  // RAX
    EXPECT_EQ(inst.operands[1].reg, 3);  // RBX
}

TEST(Decoder, AddR32RM32) {
    // 03 03 - ADD EAX, [RBX]
    auto inst = decode({0x03, 0x03});
    EXPECT_EQ(inst.opcode, Opcode::ADD_R32_RM32);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operands[0].reg, 0);  // EAX
    EXPECT_EQ(inst.operands[1].mem.base, 3);  // RBX
}

// ============================================================================
// SUB Tests
// ============================================================================

TEST(Decoder, SubAlImm8) {
    // 2C 10 - SUB AL, 0x10
    auto inst = decode({0x2C, 0x10});
    EXPECT_EQ(inst.opcode, Opcode::SUB_AL_IMM8);
    EXPECT_EQ(inst.length, 2);
}

TEST(Decoder, SubRM64R64) {
    // 48 29 D8 - SUB RAX, RBX
    auto inst = decode({0x48, 0x29, 0xD8});
    EXPECT_EQ(inst.opcode, Opcode::SUB_RM64_R64);
    EXPECT_EQ(inst.length, 3);
}

// ============================================================================
// PUSH/POP Tests
// ============================================================================

TEST(Decoder, PushR64) {
    // 50 - PUSH RAX (in 64-bit mode)
    auto inst = decode({0x50});
    EXPECT_EQ(inst.opcode, Opcode::PUSH_R64);
    EXPECT_EQ(inst.length, 1);
    EXPECT_EQ(inst.operands[0].reg, 0);  // RAX
}

TEST(Decoder, PushR64Extended) {
    // 41 50 - PUSH R8 (REX.B extends register)
    auto inst = decode({0x41, 0x50});
    EXPECT_EQ(inst.opcode, Opcode::PUSH_R64);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operands[0].reg, 8);  // R8
}

TEST(Decoder, PopR64) {
    // 58 - POP RAX
    auto inst = decode({0x58});
    EXPECT_EQ(inst.opcode, Opcode::POP_R64);
    EXPECT_EQ(inst.length, 1);
    EXPECT_EQ(inst.operands[0].reg, 0);  // RAX
}

TEST(Decoder, PopRBP) {
    // 5D - POP RBP
    auto inst = decode({0x5D});
    EXPECT_EQ(inst.opcode, Opcode::POP_R64);
    EXPECT_EQ(inst.operands[0].reg, 5);  // RBP
}

// ============================================================================
// JMP Tests
// ============================================================================

TEST(Decoder, JmpRel8) {
    // EB 10 - JMP +0x10
    auto inst = decode({0xEB, 0x10});
    EXPECT_EQ(inst.opcode, Opcode::JMP_REL8);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operands[0].imm, 0x10);
}

TEST(Decoder, JmpRel8Negative) {
    // EB FE - JMP -2 (infinite loop)
    auto inst = decode({0xEB, 0xFE});
    EXPECT_EQ(inst.opcode, Opcode::JMP_REL8);
    EXPECT_EQ(inst.operands[0].imm, -2);
}

TEST(Decoder, JmpRel32) {
    // E9 FB FF FF FF - JMP -5
    auto inst = decode({0xE9, 0xFB, 0xFF, 0xFF, 0xFF});
    EXPECT_EQ(inst.opcode, Opcode::JMP_REL32);
    EXPECT_EQ(inst.length, 5);
    EXPECT_EQ(inst.operands[0].imm, -5);
}

// ============================================================================
// Jcc Tests
// ============================================================================

TEST(Decoder, JzRel8) {
    // 74 10 - JZ +0x10
    auto inst = decode({0x74, 0x10});
    EXPECT_EQ(inst.opcode, Opcode::JZ_REL8);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operands[0].imm, 0x10);
}

TEST(Decoder, JnzRel8) {
    // 75 10 - JNZ +0x10
    auto inst = decode({0x75, 0x10});
    EXPECT_EQ(inst.opcode, Opcode::JNZ_REL8);
}

TEST(Decoder, JzRel32) {
    // 0F 84 00 01 00 00 - JZ +0x100
    auto inst = decode({0x0F, 0x84, 0x00, 0x01, 0x00, 0x00});
    EXPECT_EQ(inst.opcode, Opcode::JZ_REL32);
    EXPECT_EQ(inst.length, 6);
    EXPECT_EQ(inst.operands[0].imm, 0x100);
}

TEST(Decoder, JnzRel32) {
    // 0F 85 00 01 00 00 - JNZ +0x100
    auto inst = decode({0x0F, 0x85, 0x00, 0x01, 0x00, 0x00});
    EXPECT_EQ(inst.opcode, Opcode::JNZ_REL32);
}

// ============================================================================
// CALL/RET Tests
// ============================================================================

TEST(Decoder, CallRel32) {
    // E8 00 10 00 00 - CALL +0x1000
    auto inst = decode({0xE8, 0x00, 0x10, 0x00, 0x00});
    EXPECT_EQ(inst.opcode, Opcode::CALL_REL32);
    EXPECT_EQ(inst.length, 5);
    EXPECT_EQ(inst.operands[0].imm, 0x1000);
}

TEST(Decoder, RetNear) {
    // C3 - RET
    auto inst = decode({0xC3});
    EXPECT_EQ(inst.opcode, Opcode::RET_NEAR);
    EXPECT_EQ(inst.length, 1);
    EXPECT_EQ(inst.operand_count, 0);
}

TEST(Decoder, RetNearImm16) {
    // C2 10 00 - RET 0x10
    auto inst = decode({0xC2, 0x10, 0x00});
    EXPECT_EQ(inst.opcode, Opcode::RET_NEAR_IMM16);
    EXPECT_EQ(inst.length, 3);
    EXPECT_EQ(inst.operands[0].imm, 0x10);
}

TEST(Decoder, Retf) {
    // CB - RETF
    auto inst = decode({0xCB});
    EXPECT_EQ(inst.opcode, Opcode::RET_FAR);
    EXPECT_EQ(inst.length, 1);
    EXPECT_EQ(inst.operand_count, 0);
}

TEST(Decoder, RetfImm16) {
    // CA 08 00 - RETF 8
    auto inst = decode({0xCA, 0x08, 0x00});
    EXPECT_EQ(inst.opcode, Opcode::RET_FAR_IMM16);
    EXPECT_EQ(inst.length, 3);
    EXPECT_EQ(inst.operands[0].imm, 0x08);
}

TEST(Decoder, IretdMode32) {
    // CF - IRETD (32-bit default operand size)
    auto inst = decode({0xCF}, DecodeMode::Mode32);
    EXPECT_EQ(inst.opcode, Opcode::IRETD);
    EXPECT_EQ(inst.length, 1);
    EXPECT_EQ(inst.operand_count, 0);
}

TEST(Decoder, IretqMode64) {
    // CF - IRETQ in 64-bit mode
    auto inst = decode({0xCF}, DecodeMode::Mode64);
    EXPECT_EQ(inst.opcode, Opcode::IRETQ);
    EXPECT_EQ(inst.length, 1);
    EXPECT_EQ(inst.operand_count, 0);
}

TEST(Decoder, JmpPtr16_32Mode32) {
    // EA 78 56 34 12 00 08 — JMP 0800:12345678
    auto inst = decode({0xEA, 0x78, 0x56, 0x34, 0x12, 0x00, 0x08},
                       DecodeMode::Mode32);
    EXPECT_EQ(inst.opcode, Opcode::JMP_PTR16_32);
    EXPECT_EQ(inst.length, 7);
    ASSERT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Imm32);
    EXPECT_EQ(inst.operands[0].imm, 0x12345678);
    EXPECT_EQ(inst.operands[1].type, OperandType::Imm16);
    EXPECT_EQ(inst.operands[1].imm, 0x0800);
}

TEST(Decoder, CallPtr16_32Mode32) {
    // 9A 78 56 34 12 00 08 — CALL 0800:12345678
    auto inst = decode({0x9A, 0x78, 0x56, 0x34, 0x12, 0x00, 0x08},
                       DecodeMode::Mode32);
    EXPECT_EQ(inst.opcode, Opcode::CALL_PTR16_32);
    EXPECT_EQ(inst.length, 7);
    ASSERT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].imm, 0x12345678);
    EXPECT_EQ(inst.operands[1].imm, 0x0800);
}

TEST(Decoder, JmpPtr16_16OperandSizeOverride) {
    // 66 EA 34 12 00 08 — JMP 0800:1234
    auto inst = decode({0x66, 0xEA, 0x34, 0x12, 0x00, 0x08},
                       DecodeMode::Mode32);
    EXPECT_EQ(inst.opcode, Opcode::JMP_PTR16_16);
    EXPECT_EQ(inst.length, 6);
    ASSERT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Imm16);
    EXPECT_EQ(inst.operands[0].imm, 0x1234);
    EXPECT_EQ(inst.operands[1].imm, 0x0800);
}

TEST(Decoder, FarPtrInvalidInMode64) {
    std::vector<u8> jmp{0xEA, 0x78, 0x56, 0x34, 0x12, 0x00, 0x08};
    MemoryByteReader jmp_reader(jmp.data(), jmp.size());
    DecodedInstruction jmp_inst;
    EXPECT_EQ(decode_instruction(jmp_reader, DecodeMode::Mode64, jmp_inst),
              DecodeError::InvalidOpcode);

    std::vector<u8> call{0x9A, 0x78, 0x56, 0x34, 0x12, 0x00, 0x08};
    MemoryByteReader call_reader(call.data(), call.size());
    DecodedInstruction call_inst;
    EXPECT_EQ(decode_instruction(call_reader, DecodeMode::Mode64, call_inst),
              DecodeError::InvalidOpcode);
}

// Intel SDM Vol. 2 PUSHA/PUSHAD and POPA/POPAD: "64-bit Mode: Invalid".
// Decoding them in long mode let the interpreter run a 32-bit PUSHAD against a
// 64-bit stack — eight 4-byte slots pushed and seven registers truncated on the
// way back — instead of raising #UD.
TEST(Decoder, PushaPopaInvalidInMode64) {
    for (u8 op : {static_cast<u8>(0x60), static_cast<u8>(0x61)}) {
        std::vector<u8> code{op};
        MemoryByteReader reader(code.data(), code.size());
        DecodedInstruction inst;
        EXPECT_EQ(decode_instruction(reader, DecodeMode::Mode64, inst),
                  DecodeError::InvalidOpcode) << "opcode 0x" << std::hex
                                              << static_cast<int>(op);
    }
}

// ...but they remain valid in 32-bit mode.
TEST(Decoder, PushaPopaValidInMode32) {
    std::vector<u8> pusha{0x60};
    MemoryByteReader pusha_reader(pusha.data(), pusha.size());
    DecodedInstruction pusha_inst;
    EXPECT_EQ(decode_instruction(pusha_reader, DecodeMode::Mode32, pusha_inst),
              DecodeError::Success);
    EXPECT_EQ(pusha_inst.opcode, Opcode::PUSHA);

    std::vector<u8> popa{0x61};
    MemoryByteReader popa_reader(popa.data(), popa.size());
    DecodedInstruction popa_inst;
    EXPECT_EQ(decode_instruction(popa_reader, DecodeMode::Mode32, popa_inst),
              DecodeError::Success);
    EXPECT_EQ(popa_inst.opcode, Opcode::POPA);
}

TEST(Decoder, JmpFarMemMode32) {
    // FF /5 [disp32] — JMP m16:32
    auto inst = decode({0xFF, 0x2D, 0x00, 0x00, 0x00, 0x00},
                       DecodeMode::Mode32);
    EXPECT_EQ(inst.opcode, Opcode::JMP_M16_32);
    EXPECT_EQ(inst.length, 6);
    ASSERT_EQ(inst.operand_count, 1);
    EXPECT_TRUE(is_memory_operand(inst.operands[0].type));
}

TEST(Decoder, CallFarMemMode32) {
    // FF /3 [disp32] — CALL m16:32
    auto inst = decode({0xFF, 0x1D, 0x00, 0x00, 0x00, 0x00},
                       DecodeMode::Mode32);
    EXPECT_EQ(inst.opcode, Opcode::CALL_M16_32);
    EXPECT_EQ(inst.length, 6);
    ASSERT_EQ(inst.operand_count, 1);
    EXPECT_TRUE(is_memory_operand(inst.operands[0].type));
}

TEST(Decoder, JmpFarMemMode64) {
    // FF /5 RIP-relative — JMP m16:64 (64-bit default)
    auto inst = decode({0xFF, 0x2D, 0x00, 0x00, 0x00, 0x00});
    EXPECT_EQ(inst.opcode, Opcode::JMP_M16_64);
    EXPECT_TRUE(is_memory_operand(inst.operands[0].type));
}

TEST(Decoder, JmpFarMem16Mode64OperandSize) {
    // 66 FF /5 — JMP m16:16 in 64-bit mode
    auto inst = decode({0x66, 0xFF, 0x2D, 0x00, 0x00, 0x00, 0x00});
    EXPECT_EQ(inst.opcode, Opcode::JMP_M16_16);
}

TEST(Decoder, CallFarRegisterIsUd) {
    // FF D8 — CALL far EAX (FF /3 register) is #UD
    std::vector<u8> data{0xFF, 0xD8};
    MemoryByteReader reader(data.data(), data.size());
    DecodedInstruction inst;
    EXPECT_EQ(decode_instruction(reader, DecodeMode::Mode32, inst),
              DecodeError::InvalidOpcode);

    MemoryByteReader reader64(data.data(), data.size());
    DecodedInstruction inst64;
    EXPECT_EQ(decode_instruction(reader64, DecodeMode::Mode64, inst64),
              DecodeError::InvalidOpcode);
}

TEST(Decoder, JmpFarRegisterIsUd) {
    // FF E8 — JMP far EAX (FF /5 register) is #UD
    std::vector<u8> data{0xFF, 0xE8};
    MemoryByteReader reader(data.data(), data.size());
    DecodedInstruction inst;
    EXPECT_EQ(decode_instruction(reader, DecodeMode::Mode32, inst),
              DecodeError::InvalidOpcode);
}

// ============================================================================
// LEA Tests
// ============================================================================

TEST(Decoder, LeaR64M) {
    // 48 8D 43 10 - LEA RAX, [RBX+0x10]
    auto inst = decode({0x48, 0x8D, 0x43, 0x10});
    EXPECT_EQ(inst.opcode, Opcode::LEA_R64_M);
    EXPECT_EQ(inst.length, 4);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 0);  // RAX
    EXPECT_EQ(inst.operands[1].mem.base, 3);  // RBX
    EXPECT_EQ(inst.operands[1].mem.disp, 0x10);
}

TEST(Decoder, LeaR32M) {
    // 8D 04 88 - LEA EAX, [RAX + RCX*4]
    auto inst = decode({0x8D, 0x04, 0x88});
    EXPECT_EQ(inst.opcode, Opcode::LEA_R32_M);
    EXPECT_EQ(inst.operands[0].reg, 0);  // EAX
    EXPECT_EQ(inst.operands[1].mem.base, 0);   // RAX
    EXPECT_EQ(inst.operands[1].mem.index, 1);  // RCX
    EXPECT_EQ(inst.operands[1].mem.scale, 4);
}

// ============================================================================
// XOR Tests
// ============================================================================

TEST(Decoder, XorRM32R32) {
    // 31 C0 - XOR EAX, EAX
    auto inst = decode({0x31, 0xC0});
    EXPECT_EQ(inst.opcode, Opcode::XOR_RM32_R32);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operands[0].reg, 0);  // EAX
    EXPECT_EQ(inst.operands[1].reg, 0);  // EAX
}

TEST(Decoder, XorRM64R64) {
    // 48 31 C0 - XOR RAX, RAX
    auto inst = decode({0x48, 0x31, 0xC0});
    EXPECT_EQ(inst.opcode, Opcode::XOR_RM64_R64);
    EXPECT_EQ(inst.length, 3);
}

// ============================================================================
// CMP/TEST Tests
// ============================================================================

TEST(Decoder, CmpRM64R64) {
    // 48 39 D8 - CMP RAX, RBX
    auto inst = decode({0x48, 0x39, 0xD8});
    EXPECT_EQ(inst.opcode, Opcode::CMP_RM64_R64);
}

TEST(Decoder, TestRM64R64) {
    // 48 85 C0 - TEST RAX, RAX
    auto inst = decode({0x48, 0x85, 0xC0});
    EXPECT_EQ(inst.opcode, Opcode::TEST_RM64_R64);
}

// ============================================================================
// MOVZX/MOVSX Tests
// ============================================================================

TEST(Decoder, MovzxR32RM8) {
    // 0F B6 C1 - MOVZX EAX, CL
    auto inst = decode({0x0F, 0xB6, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::MOVZX_R32_RM8);
    EXPECT_EQ(inst.length, 3);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 0);  // EAX
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[1].reg, 1);  // CL
}

TEST(Decoder, MovsxR64RM8) {
    // 48 0F BE C1 - MOVSX RAX, CL
    auto inst = decode({0x48, 0x0F, 0xBE, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::MOVSX_R64_RM8);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
}

// ============================================================================
// Misc Tests
// ============================================================================

TEST(Decoder, Int3) {
    // CC - INT 3
    auto inst = decode({0xCC});
    EXPECT_EQ(inst.opcode, Opcode::INT_3);
    EXPECT_EQ(inst.length, 1);
}

TEST(Decoder, IntImm8) {
    // CD 80 - INT 0x80
    auto inst = decode({0xCD, 0x80});
    EXPECT_EQ(inst.opcode, Opcode::INT_IMM8);
    EXPECT_EQ(inst.operands[0].imm, 0x80);
}

TEST(Decoder, Into) {
    // CE - INTO (valid in 32/16-bit; Intel SDM INT n/INTO/INT3)
    auto inst = decode({0xCE}, DecodeMode::Mode32);
    EXPECT_EQ(inst.opcode, Opcode::INTO);
    EXPECT_EQ(inst.length, 1);
}

TEST(Decoder, IntoUdMode64) {
    // CE is #UD in 64-bit long mode
    const std::vector<u8> data{0xCE};
    MemoryByteReader reader(data.data(), data.size());
    DecodedInstruction inst;
    DecodeError err = decode_instruction(reader, DecodeMode::Mode64, inst);
    EXPECT_EQ(err, DecodeError::InvalidOpcode);
}

TEST(Decoder, Ud2) {
    // 0F 0B - UD2
    auto inst = decode({0x0F, 0x0B});
    EXPECT_EQ(inst.opcode, Opcode::UD2);
}

TEST(Decoder, Cpuid) {
    // 0F A2 - CPUID
    auto inst = decode({0x0F, 0xA2});
    EXPECT_EQ(inst.opcode, Opcode::CPUID);
}

TEST(Decoder, Sysenter) {
    auto inst = decode({0x0F, 0x34});
    EXPECT_EQ(inst.opcode, Opcode::SYSENTER);
}

TEST(Decoder, Sysexit) {
    auto inst = decode({0x0F, 0x35});
    EXPECT_EQ(inst.opcode, Opcode::SYSEXIT);
}

TEST(Decoder, Rdtsc) {
    // 0F 31 - RDTSC
    auto inst = decode({0x0F, 0x31});
    EXPECT_EQ(inst.opcode, Opcode::RDTSC);
}

TEST(Decoder, Rdtscp) {
    // 0F 01 F9 - RDTSCP (Intel SDM Vol. 2)
    auto inst64 = decode({0x0F, 0x01, 0xF9});
    EXPECT_EQ(inst64.opcode, Opcode::RDTSCP);
    EXPECT_EQ(inst64.length, 3);

    auto inst32 = decode({0x0F, 0x01, 0xF9}, DecodeMode::Mode32);
    EXPECT_EQ(inst32.opcode, Opcode::RDTSCP);
    EXPECT_EQ(inst32.length, 3);

    // 0F 01 00 is SGDT, not implemented in this user-mode runtime.
    const std::vector<u8> sgdt{0x0F, 0x01, 0x00};
    MemoryByteReader reader(sgdt.data(), sgdt.size());
    DecodedInstruction inst;
    EXPECT_EQ(decode_instruction(reader, DecodeMode::Mode64, inst),
              DecodeError::InvalidOpcode);
}

// ============================================================================
// Prefix Tests
// ============================================================================

TEST(Decoder, OperandSizePrefix) {
    // 66 B8 34 12 - MOV AX, 0x1234 (16-bit)
    auto inst = decode({0x66, 0xB8, 0x34, 0x12});
    EXPECT_EQ(inst.opcode, Opcode::MOV_R16_IMM16);
    EXPECT_EQ(inst.length, 4);
    EXPECT_TRUE(inst.prefixes.operand_size);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg16);
    EXPECT_EQ(inst.operands[1].imm, 0x1234);
}

TEST(Decoder, RexWPrefix) {
    // 48 89 C3 - MOV RBX, RAX
    auto inst = decode({0x48, 0x89, 0xC3});
    EXPECT_TRUE(inst.prefixes.rex_w());
    EXPECT_EQ(inst.opcode, Opcode::MOV_RM64_R64);
}

TEST(Decoder, RexBPrefix) {
    // 49 89 C0 - MOV R8, RAX
    auto inst = decode({0x49, 0x89, 0xC0});
    EXPECT_TRUE(inst.prefixes.rex_b());
    EXPECT_TRUE(inst.prefixes.rex_w());
    EXPECT_EQ(inst.operands[0].reg, 8);  // R8
}

// ============================================================================
// Error Handling Tests
// ============================================================================

TEST(Decoder, EmptyInputReturnsUnexpectedEnd) {
    std::vector<u8> data;
    MemoryByteReader reader(data.data(), data.size());
    DecodedInstruction inst;
    DecodeError err = decode_instruction(reader, DecodeMode::Mode64, inst);
    EXPECT_EQ(err, DecodeError::UnexpectedEnd);
}

TEST(Decoder, InvalidOpcodeReturnsError) {
    // 0F 0C is not a valid opcode
    std::vector<u8> data = {0x0F, 0x0C};
    MemoryByteReader reader(data.data(), data.size());
    DecodedInstruction inst;
    DecodeError err = decode_instruction(reader, DecodeMode::Mode64, inst);
    EXPECT_EQ(err, DecodeError::InvalidOpcode);
}

TEST(Decoder, TruncatedInstructionReturnsUnexpectedEnd) {
    // B8 without immediate
    std::vector<u8> data = {0xB8, 0x12};  // Missing 2 more bytes for imm32
    MemoryByteReader reader(data.data(), data.size());
    DecodedInstruction inst;
    DecodeError err = decode_instruction(reader, DecodeMode::Mode64, inst);
    EXPECT_EQ(err, DecodeError::UnexpectedEnd);
}

// ============================================================================
// 32-bit Mode Tests
// ============================================================================

TEST(Decoder, Mode32PushR32) {
    // 50 - PUSH EAX (in 32-bit mode)
    auto inst = decode({0x50}, DecodeMode::Mode32);
    EXPECT_EQ(inst.opcode, Opcode::PUSH_R32);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
}

TEST(Decoder, Mode32PushPopEs) {
    auto push = decode({0x06}, DecodeMode::Mode32);
    EXPECT_EQ(push.opcode, Opcode::PUSH_ES);
    ASSERT_EQ(push.operand_count, 1);
    EXPECT_EQ(push.operands[0].type, OperandType::RegSeg);
    EXPECT_EQ(push.operands[0].reg, static_cast<u8>(SegReg::ES));

    auto pop = decode({0x07}, DecodeMode::Mode32);
    EXPECT_EQ(pop.opcode, Opcode::POP_ES);
    ASSERT_EQ(pop.operand_count, 1);
    EXPECT_EQ(pop.operands[0].reg, static_cast<u8>(SegReg::ES));
}

TEST(Decoder, Mode32MovR32Imm32) {
    // B8 78 56 34 12 - MOV EAX, 0x12345678 (32-bit mode)
    auto inst = decode({0xB8, 0x78, 0x56, 0x34, 0x12}, DecodeMode::Mode32);
    EXPECT_EQ(inst.opcode, Opcode::MOV_R32_IMM32);
}

// ============================================================================
// Indirect CALL through an absolute address (the IAT call form)
// ============================================================================
//
// MSVC emits every imported call as `call dword ptr [__imp__Fn]`:
//   FF /2 with mod=00, rm=101 -> absolute disp32 in 32-bit mode.
// The encoding is 6 bytes, so the return address pushed must be
// instruction_start + 6. A length of 7 would resume one byte into the next
// instruction, which is how a StarCraft/storm.dll fault presented: MulDiv
// returned correctly but the guest resumed mid-instruction at 0xa100 instead
// of 0xa0ff.

TEST(Decoder, CallIndirectAbsoluteDisp32Is6BytesMode32) {
    // call dword ptr [0x1502a1c4]
    auto inst = decode({0xFF, 0x15, 0xC4, 0xA1, 0x02, 0x15}, DecodeMode::Mode32);
    EXPECT_EQ(inst.length, 6);
}

TEST(Decoder, JmpIndirectAbsoluteDisp32Is6BytesMode32) {
    // jmp dword ptr [0x1502a1c4]  (FF /4, same addressing form)
    auto inst = decode({0xFF, 0x25, 0xC4, 0xA1, 0x02, 0x15}, DecodeMode::Mode32);
    EXPECT_EQ(inst.length, 6);
}

TEST(Decoder, CallIndirectSibDisp32Is7BytesMode32) {
    // call dword ptr [eax*4 + 0x150096d4] — SIB form is one byte longer.
    auto inst = decode({0xFF, 0x14, 0x85, 0xD4, 0x96, 0x00, 0x15},
                       DecodeMode::Mode32);
    EXPECT_EQ(inst.length, 7);
}

// ============================================================================
// Mode-dependent opcode 63, and the undocumented-but-architectural forms
// ============================================================================
//
// These were found by the instruction-FORM coverage gate
// (jit-refactor/tools/isa_coverage.py), which compares Vitreo against Intel
// XED's ISA database at ENCODING granularity rather than by mnemonic. The
// mnemonic-level audit could not see any of them.

// Opcode 63 is two different instructions selected by CPU mode (Intel SDM
// Vol. 2): ARPL r/m16, r16 outside 64-bit mode, MOVSXD r64, r/m32 inside it.
// The decoder used to answer MOVSXD in both, so a 32-bit guest's ARPL
// performed a sign-extending move and wrote a full 32-bit destination instead
// of adjusting an RPL field and setting ZF. XED's MODES column ("mode64" on
// the MOVSXD forms) is what surfaced it.
TEST(Decoder, Opcode63IsArplOutside64Bit) {
    auto inst = decode({0x63, 0xC1}, DecodeMode::Mode32);
    EXPECT_EQ(inst.opcode, Opcode::ARPL_RM16_R16);
    EXPECT_EQ(inst.length, 2);
    EXPECT_EQ(inst.operand_count, 2);
    // Both operands are 16-bit regardless of operand-size prefix.
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg16);
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg16);
}

TEST(Decoder, Opcode63IsMovsxdIn64Bit) {
    auto inst = decode({0x63, 0xC1}, DecodeMode::Mode64);
    EXPECT_EQ(inst.opcode, Opcode::MOVSXD_R64_RM32);
    EXPECT_EQ(inst.length, 2);
}

TEST(Decoder, ArplMemoryFormLength) {
    // arpl [esp+8], cx  — ModRM + SIB + disp8
    auto inst = decode({0x63, 0x4C, 0x24, 0x08}, DecodeMode::Mode32);
    EXPECT_EQ(inst.opcode, Opcode::ARPL_RM16_R16);
    EXPECT_EQ(inst.length, 4);
    EXPECT_EQ(inst.operands[0].type, OperandType::Mem16);
}

// SALC (D6): undocumented, AL = CF ? 0xFF : 0x00. Valid outside 64-bit mode,
// #UD inside it.
TEST(Decoder, SalcDecodesOutside64BitOnly) {
    auto inst = decode({0xD6}, DecodeMode::Mode32);
    EXPECT_EQ(inst.opcode, Opcode::SALC);
    EXPECT_EQ(inst.length, 1);

    std::vector<u8> data{0xD6};
    MemoryByteReader reader(data.data(), data.size());
    DecodedInstruction in64;
    EXPECT_NE(decode_instruction(reader, DecodeMode::Mode64, in64),
              DecodeError::Success);
}

// INT1 / ICEBP (F1) raises #DB directly, unlike INT 01 (CD 01).
TEST(Decoder, Int1Decodes) {
    for (auto mode : {DecodeMode::Mode32, DecodeMode::Mode64}) {
        auto inst = decode({0xF1}, mode);
        EXPECT_EQ(inst.opcode, Opcode::INT_1);
        EXPECT_EQ(inst.length, 1);
    }
}

// FFREEP (DF C0+i) frees ST(i) AND pops; FFREE (DD C0+i) does not pop.
// Sharing one opcode between them would leave TOP one slot off for the rest
// of the block, silently renaming every later ST(i) instead of faulting.
TEST(Decoder, FfreepIsDistinctFromFfree) {
    auto ffreep = decode({0xDF, 0xC3}, DecodeMode::Mode32);
    EXPECT_EQ(ffreep.opcode, Opcode::FFREEP_STI);
    EXPECT_EQ(ffreep.length, 2);
    EXPECT_EQ(ffreep.operands[0].imm, 3);

    auto ffree = decode({0xDD, 0xC3}, DecodeMode::Mode32);
    EXPECT_EQ(ffree.opcode, Opcode::FFREE_STI);
    EXPECT_NE(ffree.opcode, ffreep.opcode);
}

// DF E0 (FNSTSW AX) and DF E8+i (FUCOMIP) share the DF map with FFREEP;
// FFREEP is reg==0, so neither may be captured by it.
TEST(Decoder, FfreepDoesNotSwallowOtherDfForms) {
    EXPECT_EQ(decode({0xDF, 0xE0}, DecodeMode::Mode32).opcode,
              Opcode::FNSTSW_AX);
    EXPECT_EQ(decode({0xDF, 0xE8}, DecodeMode::Mode32).opcode,
              Opcode::FUCOMIP_STI);
}

// UD0 (0F FF /r) and UD1 (0F B9 /r) raise the same #UD an InvalidOpcode
// decode failure would. They are decoded distinctly so the exception reports
// the length of the WHOLE instruction: these forms take a ModRM byte and may
// carry SIB and displacement, and a handler that resumes past the faulting
// instruction using the escape-byte length resumes into the middle of it.
TEST(Decoder, Ud0Ud1CarryModrmAndReportFullLength) {
    auto ud1 = decode({0x0F, 0xB9, 0xC0}, DecodeMode::Mode64);
    EXPECT_EQ(ud1.opcode, Opcode::UD1_R_RM);
    EXPECT_EQ(ud1.length, 3);

    auto ud0 = decode({0x0F, 0xFF, 0xC0}, DecodeMode::Mode64);
    EXPECT_EQ(ud0.opcode, Opcode::UD0_R_RM);
    EXPECT_EQ(ud0.length, 3);

    // ud0 eax, [rsp + 0x10] — ModRM + SIB + disp32 = 8 bytes total.
    auto ud0_disp = decode({0x0F, 0xFF, 0x84, 0x24, 0x10, 0x00, 0x00, 0x00},
                           DecodeMode::Mode64);
    EXPECT_EQ(ud0_disp.opcode, Opcode::UD0_R_RM);
    EXPECT_EQ(ud0_disp.length, 8);

    // UD2 is unchanged and takes no ModRM.
    EXPECT_EQ(decode({0x0F, 0x0B}, DecodeMode::Mode64).length, 2);
}

// The PREFETCH hint family (0F 18 /0-7, 0F 0D /0-7) already decoded, as
// NOP_RM. That is architecturally correct -- a prefetch has no observable
// state change -- and this pins it so a later edit cannot turn a legal
// instruction into #UD.
TEST(Decoder, PrefetchHintsDecodeAsNop) {
    for (u8 modrm : {0x00, 0x08, 0x10, 0x18, 0x20}) {
        EXPECT_EQ(decode({0x0F, 0x18, modrm}, DecodeMode::Mode64).opcode,
                  Opcode::NOP_RM);
    }
    EXPECT_EQ(decode({0x0F, 0x0D, 0x08}, DecodeMode::Mode64).opcode,
              Opcode::NOP_RM);
}

// ============================================================================
// EVEX prefix (Intel SDM Vol. 2, 2.6)
// ============================================================================
//
// Vitreo advertises no EVEX-encoded ISA -- AVX-512, AVX10, APX and AMX are all
// CPUID-hidden -- so every EVEX instruction raises #UD, which is what a
// processor without those features does. It is decoded anyway for two reasons
// a bare decode failure cannot serve:
//
//   * the #UD is diagnosable. "An EVEX instruction we chose not to implement"
//     and "corrupt bytes" are the same event to an InvalidOpcode return, and
//     they call for opposite responses from whoever reads the log.
//   * the length is right. Before this, only the 62 escape byte was consumed
//     and the instruction reported as one byte long.

TEST(Decoder, EvexDecodesWithExactLength) {
    // vaddps zmm0, zmm1, zmm2 -- map 1, no imm8: 62 P0 P1 P2 op modrm = 6.
    auto add = decode({0x62, 0xF1, 0x74, 0x48, 0x58, 0xC2}, DecodeMode::Mode64);
    EXPECT_EQ(add.opcode, Opcode::EVEX_UNSUPPORTED);
    EXPECT_TRUE(add.prefixes.evex);
    EXPECT_EQ(add.length, 6);

    // vcmpps -- map 1 opcode C2 is one of the eight map-1 forms WITH an imm8.
    auto cmp = decode({0x62, 0xF1, 0x74, 0x48, 0xC2, 0xC2, 0x00},
                      DecodeMode::Mode64);
    EXPECT_EQ(cmp.opcode, Opcode::EVEX_UNSUPPORTED);
    EXPECT_EQ(cmp.length, 7);

    // Map 3 (0F3A) always carries an imm8.
    EXPECT_EQ(decode({0x62, 0xF3, 0xFD, 0x48, 0x00, 0xC1, 0x1B},
                     DecodeMode::Mode64).length, 7);

    // Memory forms take SIB and displacement like any other instruction.
    EXPECT_EQ(decode({0x62, 0xF1, 0x74, 0x48, 0x58, 0x41, 0x10},
                     DecodeMode::Mode64).length, 7);
    EXPECT_EQ(decode({0x62, 0xF1, 0x74, 0x48, 0x58, 0x84, 0x24,
                      0x10, 0x00, 0x00, 0x00}, DecodeMode::Mode64).length, 11);

    // mmm is THREE bits. Maps 4 (APX), 5 and 6 (AVX512-FP16) and 7 exist in
    // current silicon and hold 3,372 of the catalog's EVEX forms; reading two
    // bits takes map 5 for map 1 and mis-lengths the instruction.
    auto fp16 = decode({0x62, 0xF5, 0x7C, 0x48, 0x58, 0xC2}, DecodeMode::Mode64);
    EXPECT_EQ(fp16.opcode, Opcode::EVEX_UNSUPPORTED);
    EXPECT_EQ(fp16.length, 6);
}

// 62 is shared with BOUND and disambiguated exactly as C4/C5 are from
// LES/LDS: BOUND needs a memory operand, so the mod==11 encodings are free.
// In 64-bit mode BOUND does not exist and 62 is always EVEX.
TEST(Decoder, EvexVersusBound) {
    auto bound = decode({0x62, 0x18}, DecodeMode::Mode32);
    EXPECT_EQ(bound.opcode, Opcode::BOUND_R32_M64);
    EXPECT_FALSE(bound.prefixes.evex);
    EXPECT_EQ(bound.length, 2);

    EXPECT_EQ(decode({0x62, 0x58, 0x08}, DecodeMode::Mode32).opcode,
              Opcode::BOUND_R32_M64);
    EXPECT_EQ(decode({0x62, 0x58, 0x08}, DecodeMode::Mode32).length, 3);

    // Same bytes, 32-bit mode, but mod==11 in the P0 position: EVEX.
    auto evex32 = decode({0x62, 0xF1, 0x74, 0x48, 0x58, 0xC2},
                         DecodeMode::Mode32);
    EXPECT_EQ(evex32.opcode, Opcode::EVEX_UNSUPPORTED);
    EXPECT_EQ(evex32.length, 6);

    // In 64-bit mode P0's top bits are R/X/B, not a mod field, so a P0 that
    // would have read as "mod != 11" is still EVEX.
    auto evex64 = decode({0x62, 0x11, 0x74, 0x48, 0x58, 0xC2},
                         DecodeMode::Mode64);
    EXPECT_EQ(evex64.opcode, Opcode::EVEX_UNSUPPORTED);
    EXPECT_EQ(evex64.length, 6);
}

// A malformed EVEX is a REAL #UD and must NOT be reported as an EVEX
// instruction: calling corrupt bytes "AVX-512 we declined to implement" is
// precisely the confusion this decode exists to remove.
TEST(Decoder, MalformedEvexIsAnOrdinaryDecodeFailure) {
    const std::vector<std::vector<u8>> bad = {
        {0x62, 0xF1, 0x70, 0x48, 0x58, 0xC2},  // P1 bit 2 clear (fixed at 1)
        {0x62, 0xF0, 0x74, 0x48, 0x58, 0xC2},  // mmm == 0, not a map
        {0x62, 0xF1},                          // truncated after P0
        {0x62, 0xF1, 0x74, 0x48},              // truncated before ModRM
    };
    for (const auto& bytes : bad) {
        auto data = bytes;
        MemoryByteReader reader(data.data(), data.size());
        DecodedInstruction inst;
        EXPECT_NE(decode_instruction(reader, DecodeMode::Mode64, inst),
                  DecodeError::Success);
    }
}

// VEX is untouched by the 62 handling, including its own C4/C5 vs LES/LDS
// disambiguation that the EVEX path is modelled on.
TEST(Decoder, VexStillDecodesAfterEvex) {
    auto vex = decode({0xC5, 0xF0, 0x58, 0xC2}, DecodeMode::Mode64);
    EXPECT_EQ(vex.opcode, Opcode::ADDPS_XMM_XMMM128);
    EXPECT_TRUE(vex.prefixes.vex);
    EXPECT_FALSE(vex.prefixes.evex);

    EXPECT_EQ(decode({0xC4, 0x18}, DecodeMode::Mode32).opcode,
              Opcode::LES_R32_M48);
}

// The new opcodes exist to be told apart from something, so a "?" in a fault
// log defeats them.
TEST(Decoder, LateAdditionsHaveNames) {
    for (Opcode op : {Opcode::ARPL_RM16_R16, Opcode::SALC, Opcode::FFREEP_STI,
                      Opcode::INT_1, Opcode::UD1_R_RM, Opcode::UD0_R_RM,
                      Opcode::EVEX_UNSUPPORTED}) {
        EXPECT_STRNE(opcode_name(op), "?")
            << "opcode " << static_cast<unsigned>(op) << " has no name";
    }
}

// ============================================================================
// opcode_name coverage
// ============================================================================
//
// `opcode_name` returns hand-written readable forms ("MOV r32, imm32") where
// someone wrote one, and used to return "?" otherwise -- for 807 of 1365
// entries (59.1%). That made the diagnostics that matter unreadable: the Tier 1
// IR coverage histogram's "top blocking opcodes" list, which decides what to
// lower next, had `?` as its top entry, and reading it meant counting enum
// entries in a header by hand.
//
// A generated identifier table (tools/gen_opcode_names.py) is the fallback.
// These tests pin both properties it has to have.

TEST(Decoder, EveryOpcodeHasAName) {
    unsigned unnamed = 0;
    for (unsigned i = 0; i < static_cast<unsigned>(Opcode::_COUNT); ++i) {
        const char* n = opcode_name(static_cast<Opcode>(i));
        ASSERT_NE(n, nullptr) << "opcode " << i;
        if (std::strcmp(n, "?") == 0) {
            if (++unnamed <= 5) ADD_FAILURE() << "opcode " << i << " has no name";
        }
    }
    EXPECT_EQ(unnamed, 0u)
        << unnamed << " opcodes print as \"?\". Regenerate the identifier "
           "table with tools/gen_opcode_names.py.";
}

// The generated table is indexed POSITIONALLY, so a dropped or reordered entry
// shifts every later name by one and the table becomes confidently wrong
// rather than obviously broken. The first generated table did exactly that --
// it omitted INVALID and reported opcode 628 as FLD_M64 when it is FLD_M32.
// These anchors are spread across the enum so a shift anywhere is caught.
TEST(Decoder, OpcodeNamesAreNotShifted) {
    struct Anchor { Opcode op; const char* id; };
    const Anchor anchors[] = {
        {Opcode::INVALID,            "INVALID"},
        {Opcode::FLD_M32,            "FLD_M32"},
        {Opcode::FSTP_M32,           "FSTP_M32"},
        {Opcode::DIV_RM32,           "DIV_RM32"},
        {Opcode::IMUL_RM32,          "IMUL_RM32"},
        {Opcode::SETZ_RM8,           "SETZ_RM8"},
        {Opcode::SHL_RM32_CL,        "SHL_RM32_CL"},
        {Opcode::MOVAPS_XMM_XMMM128, "MOVAPS_XMM_XMMM128"},
        {Opcode::EVEX_UNSUPPORTED,   "EVEX_UNSUPPORTED"},
    };
    for (const auto& a : anchors) {
        const char* n = opcode_name(a.op);
        // A hand-written readable form is fine and preferred; what must never
        // happen is the FALLBACK naming a different instruction.
        const bool pretty = std::strchr(n, ' ') != nullptr;
        if (!pretty) {
            EXPECT_STREQ(n, a.id)
                << "generated identifier table is shifted at opcode "
                << static_cast<unsigned>(a.op);
        }
    }
}

// ============================================================================
// VEX.L legality and phantom encodings, every row measured on an i9-12900KF
// by tools/oracle_probe/vex_scalar_round_probe.c (.expected alongside it).
// ============================================================================

namespace {
DecodeError try_decode(const std::initializer_list<u8>& bytes,
                       DecodedInstruction& inst,
                       DecodeMode mode = DecodeMode::Mode64) {
    std::vector<u8> data(bytes);
    MemoryByteReader reader(data.data(), data.size());
    return decode_instruction(reader, mode, inst);
}
}  // namespace

// Intel SDM Vol. 2A §3.1.1.2: VEX.LIG forms ignore VEX.L. Silicon executes
// each of these L=1 encodings exactly as L=0 (probe rows *_L1 code=0), so
// the decoder must accept them and hand every consumer the 128-bit form.
TEST(Decoder, VexLigScalarsDecodeWithLCleared) {
    struct Row { const char* name; std::initializer_list<u8> bytes; Opcode op; };
    const Row rows[] = {
        {"vroundsd L=1",    {0xC4, 0xE3, 0x75, 0x0B, 0xC2, 0x09}, Opcode::ROUNDSD_XMM_XMMM64_IMM8},
        {"vroundss L=1",    {0xC4, 0xE3, 0x75, 0x0A, 0xC2, 0x09}, Opcode::ROUNDSS_XMM_XMMM32_IMM8},
        {"vaddss L=1",      {0xC5, 0xF6, 0x58, 0xC2},             Opcode::ADDSS_XMM_XMMM32},
        {"vaddsd L=1",      {0xC5, 0xF7, 0x58, 0xC2},             Opcode::ADDSD_XMM_XMMM64},
        {"vfmadd213sd L=1", {0xC4, 0xE2, 0xF5, 0xA9, 0xC2},       Opcode::VFMADD213SD_XMM_XMM_XMMM64},
        {"vfmadd213ss L=1", {0xC4, 0xE2, 0x75, 0xA9, 0xC2},       Opcode::VFMADD213SS_XMM_XMM_XMMM32},
        {"vmovsd rr L=1",   {0xC5, 0xF7, 0x10, 0xC2},             Opcode::MOVSD_XMM_XMMM64},
        {"vmovss rm L=1",   {0xC5, 0xFE, 0x10, 0x03},             Opcode::MOVSS_XMM_XMMM32},
        {"vcvtsi2sd L=1",   {0xC5, 0xF7, 0x2A, 0xC0},             Opcode::CVTSI2SD_XMM_RM32},
        {"vsqrtsd L=1",     {0xC5, 0xF7, 0x51, 0xC2},             Opcode::SQRTSD_XMM_XMMM64},
        {"vcomisd L=1",     {0xC5, 0xFD, 0x2F, 0xC1},             Opcode::COMISD_XMM_XMMM64},
        {"vcmpsd L=1",      {0xC5, 0xF7, 0xC2, 0xC2, 0x01},       Opcode::CMPSD_XMM_XMMM64_IMM8},
    };
    for (const auto& r : rows) {
        SCOPED_TRACE(r.name);
        DecodedInstruction inst;
        ASSERT_EQ(try_decode(r.bytes, inst), DecodeError::Success);
        EXPECT_EQ(inst.opcode, r.op);
        EXPECT_TRUE(inst.prefixes.vex);
        EXPECT_FALSE(inst.prefixes.vex_l) << "LIG must be normalised to L=0";
        EXPECT_EQ(inst.length, r.bytes.size());
    }
    // The same forms with L=0 decode identically.
    auto l0 = decode({0xC4, 0xE3, 0x71, 0x0B, 0xC2, 0x09});
    EXPECT_EQ(l0.opcode, Opcode::ROUNDSD_XMM_XMMM64_IMM8);
    EXPECT_FALSE(l0.prefixes.vex_l);
}

// VEX.128-only forms: L=1 raised STATUS_ILLEGAL_INSTRUCTION on silicon
// (probe rows *_L1 code=c000001d). Maps 2 and 3 used to skip this check and
// fault only at execution; the ISA matrix counted each as a missing lowering.
TEST(Decoder, Vex128OnlyFormsRejectL1InEveryMap) {
    struct Row { const char* name; std::initializer_list<u8> bytes; };
    const Row rows[] = {
        {"vinsertps L=1",       {0xC4, 0xE3, 0x75, 0x21, 0xC2, 0x10}},
        {"vextractps L=1",      {0xC4, 0xE3, 0x7D, 0x17, 0xC0, 0x01}},
        {"vpextrb L=1",         {0xC4, 0xE3, 0x7D, 0x14, 0xC0, 0x01}},
        {"vpinsrd L=1",         {0xC4, 0xE3, 0x75, 0x22, 0xC0, 0x01}},
        {"vpclmulqdq L=1",      {0xC4, 0xE3, 0x75, 0x44, 0xC2, 0x00}},  // no VPCLMULQDQ in CPUID
        {"vaesenc L=1",         {0xC4, 0xE2, 0x75, 0xDC, 0xC2}},        // no VAES in CPUID
        {"vaeskeygenassist L=1",{0xC4, 0xE3, 0x7D, 0xDF, 0xC1, 0x01}},
        {"vpcmpistri L=1",      {0xC4, 0xE3, 0x7D, 0x63, 0xC1, 0x0C}},
        {"vphminposuw L=1",     {0xC4, 0xE2, 0x7D, 0x41, 0xC1}},
        {"vdppd L=1",           {0xC4, 0xE3, 0x75, 0x41, 0xC2, 0x31}},
        {"vmaskmovdqu L=1",     {0xC5, 0xFD, 0xF7, 0xC1}},
    };
    for (const auto& r : rows) {
        SCOPED_TRACE(r.name);
        DecodedInstruction inst;
        EXPECT_EQ(try_decode(r.bytes, inst), DecodeError::InvalidOpcode);
    }
    // And their L=0 forms still decode.
    EXPECT_EQ(decode({0xC4, 0xE3, 0x71, 0x21, 0xC2, 0x10}).opcode,
              Opcode::INSERTPS_XMM_XMMM32_IMM8);
    EXPECT_EQ(decode({0xC4, 0xE2, 0x71, 0xDC, 0xC2}).opcode,
              Opcode::AESENC_XMM_XMMM128);
    EXPECT_EQ(decode({0xC4, 0xE3, 0x71, 0x44, 0xC2, 0x00}).opcode,
              Opcode::PCLMULQDQ_XMM_XMMM128_IMM8);
}

// Intel SDM Vol. 2: the SHA extensions have only NP 0F 38/3A encodings.
TEST(Decoder, ShaHasNoVexForm) {
    DecodedInstruction inst;
    EXPECT_EQ(try_decode({0xC4, 0xE2, 0x78, 0xCB, 0xC1}, inst), DecodeError::InvalidOpcode);
    EXPECT_EQ(try_decode({0xC4, 0xE3, 0x78, 0xCC, 0xC1, 0x01}, inst), DecodeError::InvalidOpcode);
    EXPECT_EQ(try_decode({0xC4, 0xE2, 0x78, 0xCC, 0xC1}, inst), DecodeError::InvalidOpcode);
    EXPECT_EQ(try_decode({0xC4, 0xE2, 0x78, 0xC8, 0xC1}, inst), DecodeError::InvalidOpcode);
    EXPECT_EQ(decode({0x0F, 0x38, 0xCB, 0xC1}).opcode, Opcode::SHA256RNDS2_XMM_XMMM128);
    EXPECT_EQ(decode({0x0F, 0x3A, 0xCC, 0xC1, 0x01}).opcode, Opcode::SHA1RNDS4_XMM_XMMM128_IMM8);
    EXPECT_EQ(decode({0x0F, 0x38, 0xCC, 0xC1}).opcode, Opcode::SHA256MSG1_XMM_XMMM128);
}

// Intel SDM Vol. 2 MOVNTDQA xmm1, m128: memory source only. ModRM.mod = 11
// is #UD on silicon (probe rows movntdqa_reg / vmovntdqa_reg code=c000001d).
TEST(Decoder, MovntdqaRegisterFormIsUd) {
    DecodedInstruction inst;
    EXPECT_EQ(try_decode({0x66, 0x0F, 0x38, 0x2A, 0xC1}, inst), DecodeError::InvalidOpcode);
    EXPECT_EQ(try_decode({0xC4, 0xE2, 0x79, 0x2A, 0xC1}, inst), DecodeError::InvalidOpcode);
    EXPECT_EQ(try_decode({0x66, 0x0F, 0x38, 0x2A, 0xC1}, inst, DecodeMode::Mode32),
              DecodeError::InvalidOpcode);
    EXPECT_EQ(decode({0x66, 0x0F, 0x38, 0x2A, 0x03}).opcode, Opcode::MOVNTDQA_XMM_M128);
    auto ymm = decode({0xC4, 0xE2, 0x7D, 0x2A, 0x03});
    EXPECT_EQ(ymm.opcode, Opcode::MOVNTDQA_XMM_M128);
    EXPECT_TRUE(ymm.prefixes.vex_l);
}

// Intel SDM Vol. 2 groups 12/13/14 (0F 71/72/73 /x ib): the operand is a
// register; a memory ModRM is #UD (probe rows pslldq_mem / vpslldq_mem).
TEST(Decoder, ShiftByImmediateMemoryFormIsUd) {
    DecodedInstruction inst;
    EXPECT_EQ(try_decode({0x66, 0x0F, 0x73, 0x3B, 0x04}, inst), DecodeError::InvalidOpcode);
    EXPECT_EQ(try_decode({0xC5, 0xF9, 0x73, 0x3B, 0x04}, inst), DecodeError::InvalidOpcode);
    EXPECT_EQ(try_decode({0x66, 0x0F, 0x71, 0x13, 0x02}, inst), DecodeError::InvalidOpcode);
    EXPECT_EQ(try_decode({0x66, 0x0F, 0x72, 0x23, 0x02}, inst), DecodeError::InvalidOpcode);
    EXPECT_EQ(try_decode({0x0F, 0x71, 0x13, 0x02}, inst), DecodeError::InvalidOpcode);  // MMX
    EXPECT_EQ(try_decode({0x66, 0x0F, 0x73, 0x3B, 0x04}, inst, DecodeMode::Mode32),
              DecodeError::InvalidOpcode);
    EXPECT_EQ(decode({0x66, 0x0F, 0x73, 0xFB, 0x04}).opcode, Opcode::PSLLDQ_XMM_IMM8);
    EXPECT_EQ(decode({0x66, 0x0F, 0x71, 0xD1, 0x02}).opcode, Opcode::PSRLW_XMM_IMM8);
    EXPECT_EQ(decode({0x0F, 0x71, 0xD1, 0x02}).opcode, Opcode::PSRLW_MM_IMM8);
    auto ndd = decode({0xC5, 0xF9, 0x73, 0xFB, 0x04});
    EXPECT_EQ(ndd.opcode, Opcode::PSLLDQ_XMM_IMM8);
    EXPECT_TRUE(ndd.prefixes.vex);
}
