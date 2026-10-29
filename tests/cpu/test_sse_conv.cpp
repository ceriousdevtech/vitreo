// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// SSE conversion instruction tests (CPU-033)

#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include <cstring>
#include <limits>

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

TEST(SseConvDecoder, Cvtsi2ssXmm0Ecx) {
    // F3 0F 2A C1 — CVTSI2SS xmm0, ecx
    auto inst = decode({0xF3, 0x0F, 0x2A, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::CVTSI2SS_XMM_RM32);
    EXPECT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg128);
    EXPECT_EQ(inst.operands[0].reg, 0);
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].reg, 1);  // ECX
}

TEST(SseConvDecoder, Cvtsi2sdXmm0Ecx) {
    // F2 0F 2A C1 — CVTSI2SD xmm0, ecx
    auto inst = decode({0xF2, 0x0F, 0x2A, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::CVTSI2SD_XMM_RM32);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg128);
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg32);
}

TEST(SseConvDecoder, Cvtss2siEcxXmm0) {
    // F3 0F 2D C8 — CVTSS2SI ecx, xmm0
    auto inst = decode({0xF3, 0x0F, 0x2D, 0xC8});
    EXPECT_EQ(inst.opcode, Opcode::CVTSS2SI_R32_XMMM32);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 1);  // ECX
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg128);
    EXPECT_EQ(inst.operands[1].reg, 0);  // XMM0
}

TEST(SseConvDecoder, Cvttss2siEcxXmm0) {
    // F3 0F 2C C8 — CVTTSS2SI ecx, xmm0
    auto inst = decode({0xF3, 0x0F, 0x2C, 0xC8});
    EXPECT_EQ(inst.opcode, Opcode::CVTTSS2SI_R32_XMMM32);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg128);
}

TEST(SseConvDecoder, Cvtss2sdXmm0Xmm1) {
    // F3 0F 5A C1 — CVTSS2SD xmm0, xmm1
    auto inst = decode({0xF3, 0x0F, 0x5A, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::CVTSS2SD_XMM_XMMM32);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg128);
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg128);
}

TEST(SseConvDecoder, Cvtps2dqXmm0Xmm1) {
    // 66 0F 5B C1 — CVTPS2DQ xmm0, xmm1
    auto inst = decode({0x66, 0x0F, 0x5B, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::CVTPS2DQ_XMM_XMMM128);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg128);
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg128);
}

TEST(SseConvDecoder, Cvtdq2psXmm0Xmm1) {
    // 0F 5B C1 — CVTDQ2PS xmm0, xmm1
    auto inst = decode({0x0F, 0x5B, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::CVTDQ2PS_XMM_XMMM128);
}

TEST(SseConvDecoder, Cvtpd2dqXmm0Xmm1) {
    // F2 0F E6 C1 — CVTPD2DQ xmm0, xmm1
    auto inst = decode({0xF2, 0x0F, 0xE6, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::CVTPD2DQ_XMM_XMMM128);
}

TEST(SseConvDecoder, Cvtdq2pdXmm0Xmm1) {
    // F3 0F E6 C1 — CVTDQ2PD xmm0, xmm1
    auto inst = decode({0xF3, 0x0F, 0xE6, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::CVTDQ2PD_XMM_XMMM64);
}

TEST(SseConvDecoder, Cvttpd2dqXmm0Xmm1) {
    // 66 0F E6 C1 — CVTTPD2DQ xmm0, xmm1
    auto inst = decode({0x66, 0x0F, 0xE6, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::CVTTPD2DQ_XMM_XMMM128);
}

// ============================================================================
// Interpreter Test Fixture
// ============================================================================

class SseConvTest : public ::testing::Test {
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
// 1. CVTSI2SS: int 42 → float 42.0f
// ============================================================================
TEST_F(SseConvTest, Cvtsi2ssInt42) {
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::ECX), 42);
    // Pre-fill upper lanes to check they're preserved
    m_cpu->sse.xmm[0].f32_[1] = 99.0f;
    m_cpu->sse.xmm[0].f32_[2] = 88.0f;
    m_cpu->sse.xmm[0].f32_[3] = 77.0f;

    u8 code[] = { 0xF3, 0x0F, 0x2A, 0xC1 };  // CVTSI2SS xmm0, ecx
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[0], 42.0f);
    // Upper lanes preserved
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[1], 99.0f);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[2], 88.0f);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[3], 77.0f);
}

// ============================================================================
// 2. CVTSI2SD: int 42 → double 42.0
// ============================================================================
TEST_F(SseConvTest, Cvtsi2sdInt42) {
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::ECX), 42);
    // Pre-fill upper 64 bits to check they're preserved
    m_cpu->sse.xmm[0].f64_[1] = 999.0;

    u8 code[] = { 0xF2, 0x0F, 0x2A, 0xC1 };  // CVTSI2SD xmm0, ecx
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_DOUBLE_EQ(m_cpu->sse.xmm[0].f64_[0], 42.0);
    // Upper 64 bits preserved
    EXPECT_DOUBLE_EQ(m_cpu->sse.xmm[0].f64_[1], 999.0);
}

// ============================================================================
// 3. CVTSS2SI: float 3.7 → int 4 (rounds to nearest)
// ============================================================================
TEST_F(SseConvTest, Cvtss2siRoundsNearest) {
    m_cpu->sse.xmm[0].f32_[0] = 3.7f;

    // F3 0F 2D C8 — CVTSS2SI ecx, xmm0
    u8 code[] = { 0xF3, 0x0F, 0x2D, 0xC8 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(static_cast<u8>(RegIndex::ECX)), 4u);
}

// ============================================================================
// 4. CVTTSS2SI: float 3.7 → int 3 (truncates)
// ============================================================================
TEST_F(SseConvTest, Cvttss2siTruncates) {
    m_cpu->sse.xmm[0].f32_[0] = 3.7f;

    // F3 0F 2C C8 — CVTTSS2SI ecx, xmm0
    u8 code[] = { 0xF3, 0x0F, 0x2C, 0xC8 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(static_cast<u8>(RegIndex::ECX)), 3u);
}

// ============================================================================
// 5. CVTSD2SI: double 3.7 → int 4 (rounds to nearest)
// ============================================================================
TEST_F(SseConvTest, Cvtsd2siRoundsNearest) {
    m_cpu->sse.xmm[0].f64_[0] = 3.7;

    // F2 0F 2D C8 — CVTSD2SI ecx, xmm0
    u8 code[] = { 0xF2, 0x0F, 0x2D, 0xC8 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(static_cast<u8>(RegIndex::ECX)), 4u);
}

// ============================================================================
// 6. CVTTSD2SI: double 3.7 → int 3 (truncates)
// ============================================================================
TEST_F(SseConvTest, Cvttsd2siTruncates) {
    m_cpu->sse.xmm[0].f64_[0] = 3.7;

    // F2 0F 2C C8 — CVTTSD2SI ecx, xmm0
    u8 code[] = { 0xF2, 0x0F, 0x2C, 0xC8 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(static_cast<u8>(RegIndex::ECX)), 3u);
}

// ============================================================================
// 7. CVTSS2SD: float 1.5 → double 1.5
// ============================================================================
TEST_F(SseConvTest, Cvtss2sdExact) {
    m_cpu->sse.xmm[1].f32_[0] = 1.5f;

    // F3 0F 5A C1 — CVTSS2SD xmm0, xmm1
    u8 code[] = { 0xF3, 0x0F, 0x5A, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_DOUBLE_EQ(m_cpu->sse.xmm[0].f64_[0], 1.5);
}

// ============================================================================
// 8. CVTSD2SS: double 1.5 → float 1.5
// ============================================================================
TEST_F(SseConvTest, Cvtsd2ssExact) {
    m_cpu->sse.xmm[1].f64_[0] = 1.5;

    // F2 0F 5A C1 — CVTSD2SS xmm0, xmm1
    u8 code[] = { 0xF2, 0x0F, 0x5A, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[0], 1.5f);
}

// ============================================================================
// 9. CVTPS2DQ: 4×f32 packed → 4×i32
// ============================================================================
TEST_F(SseConvTest, Cvtps2dqPacked) {
    m_cpu->sse.xmm[1].f32_[0] = 1.6f;   // → 2
    m_cpu->sse.xmm[1].f32_[1] = -2.3f;  // → -2
    m_cpu->sse.xmm[1].f32_[2] = 3.5f;   // → 4 (round-to-even)
    m_cpu->sse.xmm[1].f32_[3] = 4.5f;   // → 4 (round-to-even)

    // 66 0F 5B C1 — CVTPS2DQ xmm0, xmm1
    u8 code[] = { 0x66, 0x0F, 0x5B, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].i32_[0], 2);
    EXPECT_EQ(m_cpu->sse.xmm[0].i32_[1], -2);
    EXPECT_EQ(m_cpu->sse.xmm[0].i32_[2], 4);  // banker's rounding
    EXPECT_EQ(m_cpu->sse.xmm[0].i32_[3], 4);  // banker's rounding
}

// ============================================================================
// 10. CVTDQ2PS: 4×i32 → 4×f32
// ============================================================================
TEST_F(SseConvTest, Cvtdq2psPacked) {
    m_cpu->sse.xmm[1].i32_[0] = 10;
    m_cpu->sse.xmm[1].i32_[1] = -20;
    m_cpu->sse.xmm[1].i32_[2] = 0;
    m_cpu->sse.xmm[1].i32_[3] = 12345;

    // 0F 5B C1 — CVTDQ2PS xmm0, xmm1
    u8 code[] = { 0x0F, 0x5B, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[0], 10.0f);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[1], -20.0f);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[2], 0.0f);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[3], 12345.0f);
}

// ============================================================================
// 11. Integer overflow returns 0x80000000
// ============================================================================
TEST_F(SseConvTest, Cvtss2siOverflow) {
    // +Infinity → 0x80000000
    m_cpu->sse.xmm[0].f32_[0] = std::numeric_limits<f32>::infinity();

    u8 code[] = { 0xF3, 0x0F, 0x2D, 0xC8 };  // CVTSS2SI ecx, xmm0
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(static_cast<u8>(RegIndex::ECX)), 0x80000000u);
}

TEST_F(SseConvTest, Cvtss2siNaN) {
    // NaN → 0x80000000
    m_cpu->sse.xmm[0].f32_[0] = std::numeric_limits<f32>::quiet_NaN();

    u8 code[] = { 0xF3, 0x0F, 0x2D, 0xC8 };  // CVTSS2SI ecx, xmm0
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(static_cast<u8>(RegIndex::ECX)), 0x80000000u);
}

// ============================================================================
// 12. CVTPD2PS: 2×f64 → 2×f32, upper 64 zeroed
// ============================================================================
TEST_F(SseConvTest, Cvtpd2psPacked) {
    m_cpu->sse.xmm[1].f64_[0] = 3.14;
    m_cpu->sse.xmm[1].f64_[1] = -2.718;
    // Pre-fill dst upper to verify it gets zeroed
    m_cpu->sse.xmm[0].u64_[1] = 0xDEADBEEFDEADBEEFull;

    // 66 0F 5A C1 — CVTPD2PS xmm0, xmm1
    u8 code[] = { 0x66, 0x0F, 0x5A, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[0], static_cast<f32>(3.14));
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[1], static_cast<f32>(-2.718));
    // Upper 64 bits must be zero
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[1], 0u);
}

// ============================================================================
// 13. CVTPS2PD: 2×f32 → 2×f64
// ============================================================================
TEST_F(SseConvTest, Cvtps2pdPacked) {
    m_cpu->sse.xmm[1].f32_[0] = 1.5f;
    m_cpu->sse.xmm[1].f32_[1] = -2.5f;

    // 0F 5A C1 — CVTPS2PD xmm0, xmm1
    u8 code[] = { 0x0F, 0x5A, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_DOUBLE_EQ(m_cpu->sse.xmm[0].f64_[0], 1.5);
    EXPECT_DOUBLE_EQ(m_cpu->sse.xmm[0].f64_[1], -2.5);
}

// ============================================================================
// 14. CVTPD2DQ: 2×f64 → 2×i32, upper 64 zeroed
// ============================================================================
TEST_F(SseConvTest, Cvtpd2dqPacked) {
    m_cpu->sse.xmm[1].f64_[0] = 3.7;    // → 4
    m_cpu->sse.xmm[1].f64_[1] = -1.2;   // → -1
    // Pre-fill dst upper
    m_cpu->sse.xmm[0].u64_[1] = 0xFFFFFFFFFFFFFFFFull;

    // F2 0F E6 C1 — CVTPD2DQ xmm0, xmm1
    u8 code[] = { 0xF2, 0x0F, 0xE6, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].i32_[0], 4);
    EXPECT_EQ(m_cpu->sse.xmm[0].i32_[1], -1);
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[1], 0u);
}

// ============================================================================
// 15. CVTDQ2PD: 2×i32 → 2×f64
// ============================================================================
TEST_F(SseConvTest, Cvtdq2pdPacked) {
    m_cpu->sse.xmm[1].i32_[0] = 100;
    m_cpu->sse.xmm[1].i32_[1] = -50;

    // F3 0F E6 C1 — CVTDQ2PD xmm0, xmm1
    u8 code[] = { 0xF3, 0x0F, 0xE6, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_DOUBLE_EQ(m_cpu->sse.xmm[0].f64_[0], 100.0);
    EXPECT_DOUBLE_EQ(m_cpu->sse.xmm[0].f64_[1], -50.0);
}

// ============================================================================
// 16. CVTTPS2DQ: 4×f32 → 4×i32 (truncation)
// ============================================================================
TEST_F(SseConvTest, Cvttps2dqPacked) {
    m_cpu->sse.xmm[1].f32_[0] = 1.9f;   // → 1
    m_cpu->sse.xmm[1].f32_[1] = -2.9f;  // → -2
    m_cpu->sse.xmm[1].f32_[2] = 3.1f;   // → 3
    m_cpu->sse.xmm[1].f32_[3] = -0.5f;  // → 0

    // F3 0F 5B C1 — CVTTPS2DQ xmm0, xmm1
    u8 code[] = { 0xF3, 0x0F, 0x5B, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].i32_[0], 1);
    EXPECT_EQ(m_cpu->sse.xmm[0].i32_[1], -2);
    EXPECT_EQ(m_cpu->sse.xmm[0].i32_[2], 3);
    EXPECT_EQ(m_cpu->sse.xmm[0].i32_[3], 0);
}

// ============================================================================
// 17. CVTTPD2DQ: 2×f64 → 2×i32 (truncation), upper 64 zeroed
// ============================================================================
TEST_F(SseConvTest, Cvttpd2dqPacked) {
    m_cpu->sse.xmm[1].f64_[0] = 7.9;   // → 7
    m_cpu->sse.xmm[1].f64_[1] = -3.1;  // → -3
    m_cpu->sse.xmm[0].u64_[1] = 0xDEADDEADDEADDEADull;

    // 66 0F E6 C1 — CVTTPD2DQ xmm0, xmm1
    u8 code[] = { 0x66, 0x0F, 0xE6, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].i32_[0], 7);
    EXPECT_EQ(m_cpu->sse.xmm[0].i32_[1], -3);
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[1], 0u);
}

// ============================================================================
// 18. Negative integer conversion: CVTSI2SS with -1
// ============================================================================
TEST_F(SseConvTest, Cvtsi2ssNegative) {
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::ECX),
                     static_cast<u32>(static_cast<i32>(-1)));

    u8 code[] = { 0xF3, 0x0F, 0x2A, 0xC1 };  // CVTSI2SS xmm0, ecx
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[0], -1.0f);
}

// ============================================================================
// 19. CVTPI2PS: 2×i32 from MM → 2×f32, upper 64 bits of XMM preserved
//     The Sims 3's audio mixer uses NP 0F 2A between MMX integer stages.
// ============================================================================
TEST_F(SseConvTest, Cvtpi2psFromMmx) {
    m_cpu->fpu.set_mm(1, (static_cast<u64>(static_cast<u32>(-7)) << 32) | 42u);
    m_cpu->sse.xmm[0].f32_[2] = 88.0f;
    m_cpu->sse.xmm[0].f32_[3] = 77.0f;

    u8 code[] = { 0x0F, 0x2A, 0xC1 };  // CVTPI2PS xmm0, mm1
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[0], 42.0f);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[1], -7.0f);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[2], 88.0f);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[3], 77.0f);
}

// ============================================================================
// 20. CVTPS2PI / CVTTPS2PI: 2×f32 → 2×i32 in an MM register
// ============================================================================
TEST_F(SseConvTest, Cvtps2piRoundsAndTruncates) {
    m_cpu->sse.xmm[1].f32_[0] = 3.7f;
    m_cpu->sse.xmm[1].f32_[1] = -2.5f;

    u8 round[] = { 0x0F, 0x2D, 0xC1 };  // CVTPS2PI mm0, xmm1
    write_code(round, sizeof(round));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(static_cast<i32>(static_cast<u32>(m_cpu->fpu.get_mm(0))), 4);
    EXPECT_EQ(static_cast<i32>(static_cast<u32>(m_cpu->fpu.get_mm(0) >> 32)), -2);

    m_cpu->rip = 0x1000;
    u8 trunc[] = { 0x0F, 0x2C, 0xC1 };  // CVTTPS2PI mm0, xmm1
    write_code(trunc, sizeof(trunc));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(static_cast<i32>(static_cast<u32>(m_cpu->fpu.get_mm(0))), 3);
    EXPECT_EQ(static_cast<i32>(static_cast<u32>(m_cpu->fpu.get_mm(0) >> 32)), -2);
}

// ============================================================================
// 21. CVTPI2PD / CVTTPD2PI: the 66-prefixed MMX ↔ double forms
// ============================================================================
TEST_F(SseConvTest, Cvtpi2pdAndCvttpd2pi) {
    m_cpu->fpu.set_mm(2, (static_cast<u64>(static_cast<u32>(-9)) << 32) | 5u);

    u8 to_pd[] = { 0x66, 0x0F, 0x2A, 0xC2 };  // CVTPI2PD xmm0, mm2
    write_code(to_pd, sizeof(to_pd));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_DOUBLE_EQ(m_cpu->sse.xmm[0].f64_[0], 5.0);
    EXPECT_DOUBLE_EQ(m_cpu->sse.xmm[0].f64_[1], -9.0);

    m_cpu->rip = 0x1000;
    m_cpu->sse.xmm[0].f64_[0] = 6.9;
    m_cpu->sse.xmm[0].f64_[1] = -6.9;
    u8 to_pi[] = { 0x66, 0x0F, 0x2C, 0xD8 };  // CVTTPD2PI mm3, xmm0
    write_code(to_pi, sizeof(to_pi));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(static_cast<i32>(static_cast<u32>(m_cpu->fpu.get_mm(3))), 6);
    EXPECT_EQ(static_cast<i32>(static_cast<u32>(m_cpu->fpu.get_mm(3) >> 32)), -6);
}

// ============================================================================
// 22. Decode: the MMX conversion forms name the MM register file
// ============================================================================
TEST(SseConvDecoder, Cvtpi2psDecodesMmxSource) {
    auto inst = decode({0x0F, 0x2A, 0xC1});  // CVTPI2PS xmm0, mm1
    EXPECT_EQ(inst.opcode, Opcode::CVTPI2PS_XMM_MMM64);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg128);
    EXPECT_EQ(inst.operands[0].reg, 0);
    EXPECT_EQ(inst.operands[1].type, OperandType::RegMM);
    EXPECT_EQ(inst.operands[1].reg, 1);
}

TEST(SseConvDecoder, Cvtps2piDecodesMmxDestination) {
    auto inst = decode({0x0F, 0x2D, 0xC1});  // CVTPS2PI mm0, xmm1
    EXPECT_EQ(inst.opcode, Opcode::CVTPS2PI_MM_XMMM64);
    EXPECT_EQ(inst.operands[0].type, OperandType::RegMM);
    EXPECT_EQ(inst.operands[0].reg, 0);
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg128);
    EXPECT_EQ(inst.operands[1].reg, 1);
}
