// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// SSE shuffle and unpack instruction tests (CPU-038)

#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include <gtest/gtest.h>
#include <cstring>

using namespace vitreo;
using namespace vitreo::cpu;

// ============================================================================
// Decode-level helpers
// ============================================================================

static DecodedInstruction decode(const std::initializer_list<u8>& bytes,
                                 DecodeMode mode = DecodeMode::Mode64) {
    std::vector<u8> data(bytes);
    MemoryByteReader reader(data.data(), data.size());
    DecodedInstruction inst;
    DecodeError err = decode_instruction(reader, mode, inst);
    EXPECT_EQ(err, DecodeError::Success) << "Decode failed";
    return inst;
}

// ============================================================================
// Decoder Tests
// ============================================================================

TEST(SseShuffleDecoder, ShufpsXmm0Xmm1Imm) {
    // 0F C6 C1 1B — SHUFPS xmm0, xmm1, 0x1B
    auto inst = decode({0x0F, 0xC6, 0xC1, 0x1B});
    EXPECT_EQ(inst.opcode, Opcode::SHUFPS_XMM_XMMM128_IMM8);
    EXPECT_EQ(inst.operand_count, 3);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg128);
    EXPECT_EQ(inst.operands[0].reg, 0);
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg128);
    EXPECT_EQ(inst.operands[1].reg, 1);
    EXPECT_EQ(inst.operands[2].type, OperandType::Imm8);
    EXPECT_EQ(inst.operands[2].imm, 0x1B);
}

TEST(SseShuffleDecoder, ShufpdXmm0Xmm1Imm) {
    // 66 0F C6 C1 01 — SHUFPD xmm0, xmm1, 0x01
    auto inst = decode({0x66, 0x0F, 0xC6, 0xC1, 0x01});
    EXPECT_EQ(inst.opcode, Opcode::SHUFPD_XMM_XMMM128_IMM8);
    EXPECT_EQ(inst.operand_count, 3);
    EXPECT_EQ(inst.operands[2].imm, 0x01);
}

TEST(SseShuffleDecoder, PshufdXmm0Xmm1Imm) {
    // 66 0F 70 C1 E4 — PSHUFD xmm0, xmm1, 0xE4
    auto inst = decode({0x66, 0x0F, 0x70, 0xC1, 0xE4});
    EXPECT_EQ(inst.opcode, Opcode::PSHUFD_XMM_XMMM128_IMM8);
    EXPECT_EQ(inst.operands[2].imm, 0xE4);
}

TEST(SseShuffleDecoder, PshuflwXmm0Xmm1Imm) {
    // F2 0F 70 C1 E4 — PSHUFLW xmm0, xmm1, 0xE4
    auto inst = decode({0xF2, 0x0F, 0x70, 0xC1, 0xE4});
    EXPECT_EQ(inst.opcode, Opcode::PSHUFLW_XMM_XMMM128_IMM8);
}

TEST(SseShuffleDecoder, PshufhwXmm0Xmm1Imm) {
    // F3 0F 70 C1 E4 — PSHUFHW xmm0, xmm1, 0xE4
    auto inst = decode({0xF3, 0x0F, 0x70, 0xC1, 0xE4});
    EXPECT_EQ(inst.opcode, Opcode::PSHUFHW_XMM_XMMM128_IMM8);
}

TEST(SseShuffleDecoder, UnpcklpsXmm0Xmm1) {
    // 0F 14 C1 — UNPCKLPS xmm0, xmm1
    auto inst = decode({0x0F, 0x14, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::UNPCKLPS_XMM_XMMM128);
    EXPECT_EQ(inst.operand_count, 2);
}

TEST(SseShuffleDecoder, UnpckhpdXmm0Xmm1) {
    // 66 0F 15 C1 — UNPCKHPD xmm0, xmm1
    auto inst = decode({0x66, 0x0F, 0x15, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::UNPCKHPD_XMM_XMMM128);
}

TEST(SseShuffleDecoder, PunpcklbwXmm0Xmm1) {
    // 66 0F 60 C1 — PUNPCKLBW xmm0, xmm1
    auto inst = decode({0x66, 0x0F, 0x60, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::PUNPCKLBW_XMM_XMMM128);
    EXPECT_EQ(inst.operand_count, 2);
}

// ============================================================================
// Interpreter Test Fixture
// ============================================================================

class SseShuffleTest : public ::testing::Test {
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
// 1. SHUFPS: reverse order with imm8 = 0x1B (00 01 10 11)
//    result[0]=dst[3], result[1]=dst[2], result[2]=src[1], result[3]=src[0]
// ============================================================================
TEST_F(SseShuffleTest, ShufpsReverse) {
    m_cpu->sse.xmm[0].f32_[0] = 1.0f;
    m_cpu->sse.xmm[0].f32_[1] = 2.0f;
    m_cpu->sse.xmm[0].f32_[2] = 3.0f;
    m_cpu->sse.xmm[0].f32_[3] = 4.0f;

    m_cpu->sse.xmm[1].f32_[0] = 10.0f;
    m_cpu->sse.xmm[1].f32_[1] = 20.0f;
    m_cpu->sse.xmm[1].f32_[2] = 30.0f;
    m_cpu->sse.xmm[1].f32_[3] = 40.0f;

    // imm8 = 0x1B = 00_01_10_11
    // result[0]=dst[(0x1B>>0)&3]=dst[3]=4.0
    // result[1]=dst[(0x1B>>2)&3]=dst[2]=3.0
    // result[2]=src[(0x1B>>4)&3]=src[1]=20.0
    // result[3]=src[(0x1B>>6)&3]=src[0]=10.0
    u8 code[] = { 0x0F, 0xC6, 0xC1, 0x1B };  // SHUFPS xmm0,xmm1,0x1B
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[0], 4.0f);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[1], 3.0f);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[2], 20.0f);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[3], 10.0f);
}

// ============================================================================
// 2. SHUFPS: identity with imm8 = 0xE4 (11 10 01 00)
// ============================================================================
TEST_F(SseShuffleTest, ShufpsIdentity) {
    m_cpu->sse.xmm[0].f32_[0] = 1.0f;
    m_cpu->sse.xmm[0].f32_[1] = 2.0f;
    m_cpu->sse.xmm[0].f32_[2] = 3.0f;
    m_cpu->sse.xmm[0].f32_[3] = 4.0f;

    m_cpu->sse.xmm[1].f32_[0] = 10.0f;
    m_cpu->sse.xmm[1].f32_[1] = 20.0f;
    m_cpu->sse.xmm[1].f32_[2] = 30.0f;
    m_cpu->sse.xmm[1].f32_[3] = 40.0f;

    // imm8 = 0xE4 = 11_10_01_00 => dst[0],dst[1],src[2],src[3]
    u8 code[] = { 0x0F, 0xC6, 0xC1, 0xE4 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[0], 1.0f);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[1], 2.0f);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[2], 30.0f);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[3], 40.0f);
}

// ============================================================================
// 3. SHUFPD: swap doubles with imm8=0x01
// ============================================================================
TEST_F(SseShuffleTest, ShufpdSwap) {
    m_cpu->sse.xmm[0].f64_[0] = 1.0;
    m_cpu->sse.xmm[0].f64_[1] = 2.0;
    m_cpu->sse.xmm[1].f64_[0] = 10.0;
    m_cpu->sse.xmm[1].f64_[1] = 20.0;

    // imm8=0x01 = bit0=1 => result[0]=dst[1]=2.0, bit1=0 => result[1]=src[0]=10.0
    u8 code[] = { 0x66, 0x0F, 0xC6, 0xC1, 0x01 };  // SHUFPD xmm0,xmm1,0x01
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_DOUBLE_EQ(m_cpu->sse.xmm[0].f64_[0], 2.0);
    EXPECT_DOUBLE_EQ(m_cpu->sse.xmm[0].f64_[1], 10.0);
}

// ============================================================================
// 4. PSHUFD: identity imm8=0xE4
// ============================================================================
TEST_F(SseShuffleTest, PshufdIdentity) {
    m_cpu->sse.xmm[1].u32_[0] = 0xAAAA;
    m_cpu->sse.xmm[1].u32_[1] = 0xBBBB;
    m_cpu->sse.xmm[1].u32_[2] = 0xCCCC;
    m_cpu->sse.xmm[1].u32_[3] = 0xDDDD;

    // PSHUFD xmm0, xmm1, 0xE4 (identity: 11_10_01_00)
    u8 code[] = { 0x66, 0x0F, 0x70, 0xC1, 0xE4 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[0], 0xAAAAu);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[1], 0xBBBBu);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[2], 0xCCCCu);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[3], 0xDDDDu);
}

// ============================================================================
// 5. PSHUFD: broadcast dword[0] with imm8=0x00
// ============================================================================
TEST_F(SseShuffleTest, PshufdBroadcast) {
    m_cpu->sse.xmm[1].u32_[0] = 42;
    m_cpu->sse.xmm[1].u32_[1] = 100;
    m_cpu->sse.xmm[1].u32_[2] = 200;
    m_cpu->sse.xmm[1].u32_[3] = 300;

    // PSHUFD xmm0, xmm1, 0x00 (all from element 0)
    u8 code[] = { 0x66, 0x0F, 0x70, 0xC1, 0x00 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[0], 42u);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[1], 42u);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[2], 42u);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[3], 42u);
}

// ============================================================================
// 6. PSHUFLW: shuffle low words, high unchanged
// ============================================================================
TEST_F(SseShuffleTest, PshuflwBasic) {
    for (int i = 0; i < 8; ++i)
        m_cpu->sse.xmm[1].u16_[i] = static_cast<u16>(i * 10);

    // PSHUFLW xmm0, xmm1, 0x1B (reverse low 4 words: 00_01_10_11)
    u8 code[] = { 0xF2, 0x0F, 0x70, 0xC1, 0x1B };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u16_[0], 30u);  // word[3]
    EXPECT_EQ(m_cpu->sse.xmm[0].u16_[1], 20u);  // word[2]
    EXPECT_EQ(m_cpu->sse.xmm[0].u16_[2], 10u);  // word[1]
    EXPECT_EQ(m_cpu->sse.xmm[0].u16_[3], 0u);   // word[0]
    // High 4 words copied from src
    EXPECT_EQ(m_cpu->sse.xmm[0].u16_[4], 40u);
    EXPECT_EQ(m_cpu->sse.xmm[0].u16_[5], 50u);
    EXPECT_EQ(m_cpu->sse.xmm[0].u16_[6], 60u);
    EXPECT_EQ(m_cpu->sse.xmm[0].u16_[7], 70u);
}

// ============================================================================
// 7. PSHUFHW: shuffle high words, low unchanged
// ============================================================================
TEST_F(SseShuffleTest, PshufhwBasic) {
    for (int i = 0; i < 8; ++i)
        m_cpu->sse.xmm[1].u16_[i] = static_cast<u16>(i * 10);

    // PSHUFHW xmm0, xmm1, 0x1B (reverse high 4 words)
    u8 code[] = { 0xF3, 0x0F, 0x70, 0xC1, 0x1B };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // Low 4 words copied from src
    EXPECT_EQ(m_cpu->sse.xmm[0].u16_[0], 0u);
    EXPECT_EQ(m_cpu->sse.xmm[0].u16_[1], 10u);
    EXPECT_EQ(m_cpu->sse.xmm[0].u16_[2], 20u);
    EXPECT_EQ(m_cpu->sse.xmm[0].u16_[3], 30u);
    // High shuffled: reverse of words 4,5,6,7 = 40,50,60,70
    EXPECT_EQ(m_cpu->sse.xmm[0].u16_[4], 70u);  // word[4+3]
    EXPECT_EQ(m_cpu->sse.xmm[0].u16_[5], 60u);  // word[4+2]
    EXPECT_EQ(m_cpu->sse.xmm[0].u16_[6], 50u);  // word[4+1]
    EXPECT_EQ(m_cpu->sse.xmm[0].u16_[7], 40u);  // word[4+0]
}

// ============================================================================
// 8. UNPCKLPS: interleave low floats
// ============================================================================
TEST_F(SseShuffleTest, UnpcklpsBasic) {
    m_cpu->sse.xmm[0].f32_[0] = 1.0f;
    m_cpu->sse.xmm[0].f32_[1] = 2.0f;
    m_cpu->sse.xmm[0].f32_[2] = 3.0f;
    m_cpu->sse.xmm[0].f32_[3] = 4.0f;

    m_cpu->sse.xmm[1].f32_[0] = 10.0f;
    m_cpu->sse.xmm[1].f32_[1] = 20.0f;
    m_cpu->sse.xmm[1].f32_[2] = 30.0f;
    m_cpu->sse.xmm[1].f32_[3] = 40.0f;

    u8 code[] = { 0x0F, 0x14, 0xC1 };  // UNPCKLPS xmm0, xmm1
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[0], 1.0f);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[1], 10.0f);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[2], 2.0f);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[3], 20.0f);
}

// ============================================================================
// 9. UNPCKHPS: interleave high floats
// ============================================================================
TEST_F(SseShuffleTest, UnpckhpsBasic) {
    m_cpu->sse.xmm[0].f32_[0] = 1.0f;
    m_cpu->sse.xmm[0].f32_[1] = 2.0f;
    m_cpu->sse.xmm[0].f32_[2] = 3.0f;
    m_cpu->sse.xmm[0].f32_[3] = 4.0f;

    m_cpu->sse.xmm[1].f32_[0] = 10.0f;
    m_cpu->sse.xmm[1].f32_[1] = 20.0f;
    m_cpu->sse.xmm[1].f32_[2] = 30.0f;
    m_cpu->sse.xmm[1].f32_[3] = 40.0f;

    u8 code[] = { 0x0F, 0x15, 0xC1 };  // UNPCKHPS xmm0, xmm1
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[0], 3.0f);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[1], 30.0f);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[2], 4.0f);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[3], 40.0f);
}

// ============================================================================
// 10. UNPCKLPD: interleave low doubles
// ============================================================================
TEST_F(SseShuffleTest, UnpcklpdBasic) {
    m_cpu->sse.xmm[0].f64_[0] = 1.0;
    m_cpu->sse.xmm[0].f64_[1] = 2.0;
    m_cpu->sse.xmm[1].f64_[0] = 10.0;
    m_cpu->sse.xmm[1].f64_[1] = 20.0;

    u8 code[] = { 0x66, 0x0F, 0x14, 0xC1 };  // UNPCKLPD xmm0, xmm1
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_DOUBLE_EQ(m_cpu->sse.xmm[0].f64_[0], 1.0);
    EXPECT_DOUBLE_EQ(m_cpu->sse.xmm[0].f64_[1], 10.0);
}

// ============================================================================
// 11. UNPCKHPD: interleave high doubles
// ============================================================================
TEST_F(SseShuffleTest, UnpckhpdBasic) {
    m_cpu->sse.xmm[0].f64_[0] = 1.0;
    m_cpu->sse.xmm[0].f64_[1] = 2.0;
    m_cpu->sse.xmm[1].f64_[0] = 10.0;
    m_cpu->sse.xmm[1].f64_[1] = 20.0;

    u8 code[] = { 0x66, 0x0F, 0x15, 0xC1 };  // UNPCKHPD xmm0, xmm1
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_DOUBLE_EQ(m_cpu->sse.xmm[0].f64_[0], 2.0);
    EXPECT_DOUBLE_EQ(m_cpu->sse.xmm[0].f64_[1], 20.0);
}

// ============================================================================
// 12. PUNPCKLBW: interleave low 8 bytes
// ============================================================================
TEST_F(SseShuffleTest, PunpcklbwBasic) {
    for (int i = 0; i < 16; ++i) {
        m_cpu->sse.xmm[0].u8_[i] = static_cast<u8>(i);
        m_cpu->sse.xmm[1].u8_[i] = static_cast<u8>(0x80 + i);
    }

    u8 code[] = { 0x66, 0x0F, 0x60, 0xC1 };  // PUNPCKLBW xmm0, xmm1
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // result[2i]=dst_orig[i], result[2i+1]=src[i] for i=0..7
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(m_cpu->sse.xmm[0].u8_[2 * i],     static_cast<u8>(i));
        EXPECT_EQ(m_cpu->sse.xmm[0].u8_[2 * i + 1], static_cast<u8>(0x80 + i));
    }
}

// ============================================================================
// 13. PUNPCKHBW: interleave high 8 bytes
// ============================================================================
TEST_F(SseShuffleTest, PunpckhbwBasic) {
    for (int i = 0; i < 16; ++i) {
        m_cpu->sse.xmm[0].u8_[i] = static_cast<u8>(i);
        m_cpu->sse.xmm[1].u8_[i] = static_cast<u8>(0x80 + i);
    }

    u8 code[] = { 0x66, 0x0F, 0x68, 0xC1 };  // PUNPCKHBW xmm0, xmm1
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // result[2i]=dst_orig[8+i], result[2i+1]=src[8+i] for i=0..7
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(m_cpu->sse.xmm[0].u8_[2 * i],     static_cast<u8>(8 + i));
        EXPECT_EQ(m_cpu->sse.xmm[0].u8_[2 * i + 1], static_cast<u8>(0x88 + i));
    }
}

// ============================================================================
// 14. PUNPCKLWD: interleave low 4 words
// ============================================================================
TEST_F(SseShuffleTest, PunpcklwdBasic) {
    for (int i = 0; i < 8; ++i) {
        m_cpu->sse.xmm[0].u16_[i] = static_cast<u16>(i);
        m_cpu->sse.xmm[1].u16_[i] = static_cast<u16>(0x100 + i);
    }

    u8 code[] = { 0x66, 0x0F, 0x61, 0xC1 };  // PUNPCKLWD xmm0, xmm1
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(m_cpu->sse.xmm[0].u16_[2 * i],     static_cast<u16>(i));
        EXPECT_EQ(m_cpu->sse.xmm[0].u16_[2 * i + 1], static_cast<u16>(0x100 + i));
    }
}

// ============================================================================
// 15. PUNPCKLDQ: interleave low 2 dwords
// ============================================================================
TEST_F(SseShuffleTest, PunpckldqBasic) {
    m_cpu->sse.xmm[0].u32_[0] = 0xAA;
    m_cpu->sse.xmm[0].u32_[1] = 0xBB;
    m_cpu->sse.xmm[0].u32_[2] = 0xCC;
    m_cpu->sse.xmm[0].u32_[3] = 0xDD;

    m_cpu->sse.xmm[1].u32_[0] = 0x11;
    m_cpu->sse.xmm[1].u32_[1] = 0x22;
    m_cpu->sse.xmm[1].u32_[2] = 0x33;
    m_cpu->sse.xmm[1].u32_[3] = 0x44;

    u8 code[] = { 0x66, 0x0F, 0x62, 0xC1 };  // PUNPCKLDQ xmm0, xmm1
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[0], 0xAAu);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[1], 0x11u);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[2], 0xBBu);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[3], 0x22u);
}

// ============================================================================
// 16. PUNPCKLQDQ: interleave low 2 qwords
// ============================================================================
TEST_F(SseShuffleTest, PunpcklqdqBasic) {
    m_cpu->sse.xmm[0].u64_[0] = 0xAAAA;
    m_cpu->sse.xmm[0].u64_[1] = 0xBBBB;
    m_cpu->sse.xmm[1].u64_[0] = 0x1111;
    m_cpu->sse.xmm[1].u64_[1] = 0x2222;

    u8 code[] = { 0x66, 0x0F, 0x6C, 0xC1 };  // PUNPCKLQDQ xmm0, xmm1
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[0], 0xAAAAu);
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[1], 0x1111u);
}

// ============================================================================
// 17. PUNPCKHQDQ: interleave high 2 qwords
// ============================================================================
TEST_F(SseShuffleTest, PunpckhqdqBasic) {
    m_cpu->sse.xmm[0].u64_[0] = 0xAAAA;
    m_cpu->sse.xmm[0].u64_[1] = 0xBBBB;
    m_cpu->sse.xmm[1].u64_[0] = 0x1111;
    m_cpu->sse.xmm[1].u64_[1] = 0x2222;

    u8 code[] = { 0x66, 0x0F, 0x6D, 0xC1 };  // PUNPCKHQDQ xmm0, xmm1
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[0], 0xBBBBu);
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[1], 0x2222u);
}

// ============================================================================
// 18. PUNPCKHWD: interleave high 4 words
// ============================================================================
TEST_F(SseShuffleTest, PunpckhwdBasic) {
    for (int i = 0; i < 8; ++i) {
        m_cpu->sse.xmm[0].u16_[i] = static_cast<u16>(i);
        m_cpu->sse.xmm[1].u16_[i] = static_cast<u16>(0x100 + i);
    }

    u8 code[] = { 0x66, 0x0F, 0x69, 0xC1 };  // PUNPCKHWD xmm0, xmm1
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(m_cpu->sse.xmm[0].u16_[2 * i],     static_cast<u16>(4 + i));
        EXPECT_EQ(m_cpu->sse.xmm[0].u16_[2 * i + 1], static_cast<u16>(0x104 + i));
    }
}

// ============================================================================
// 19. PUNPCKHDQ: interleave high 2 dwords
// ============================================================================
TEST_F(SseShuffleTest, PunpckhdqBasic) {
    m_cpu->sse.xmm[0].u32_[0] = 0xAA;
    m_cpu->sse.xmm[0].u32_[1] = 0xBB;
    m_cpu->sse.xmm[0].u32_[2] = 0xCC;
    m_cpu->sse.xmm[0].u32_[3] = 0xDD;

    m_cpu->sse.xmm[1].u32_[0] = 0x11;
    m_cpu->sse.xmm[1].u32_[1] = 0x22;
    m_cpu->sse.xmm[1].u32_[2] = 0x33;
    m_cpu->sse.xmm[1].u32_[3] = 0x44;

    u8 code[] = { 0x66, 0x0F, 0x6A, 0xC1 };  // PUNPCKHDQ xmm0, xmm1
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[0], 0xCCu);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[1], 0x33u);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[2], 0xDDu);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[3], 0x44u);
}
