// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// SSE arithmetic instruction tests (CPU-032)

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

TEST(SseArithDecoder, AddssXmm0Xmm1) {
    // F3 0F 58 C1 — ADDSS xmm0, xmm1
    auto inst = decode({0xF3, 0x0F, 0x58, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::ADDSS_XMM_XMMM32);
    EXPECT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg128);
    EXPECT_EQ(inst.operands[0].reg, 0);
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg128);
    EXPECT_EQ(inst.operands[1].reg, 1);
}

TEST(SseArithDecoder, AddsdXmm0Xmm1) {
    // F2 0F 58 C1 — ADDSD xmm0, xmm1
    auto inst = decode({0xF2, 0x0F, 0x58, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::ADDSD_XMM_XMMM64);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg128);
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg128);
}

TEST(SseArithDecoder, AddpsXmm0Xmm1) {
    // 0F 58 C1 — ADDPS xmm0, xmm1
    auto inst = decode({0x0F, 0x58, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::ADDPS_XMM_XMMM128);
}

TEST(SseArithDecoder, AddpdXmm0Xmm1) {
    // 66 0F 58 C1 — ADDPD xmm0, xmm1
    auto inst = decode({0x66, 0x0F, 0x58, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::ADDPD_XMM_XMMM128);
}

TEST(SseArithDecoder, ComissXmm0Xmm1) {
    // 0F 2F C1 — COMISS xmm0, xmm1
    auto inst = decode({0x0F, 0x2F, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::COMISS_XMM_XMMM32);
}

TEST(SseArithDecoder, SqrtssXmm0Xmm1) {
    // F3 0F 51 C1 — SQRTSS xmm0, xmm1
    auto inst = decode({0xF3, 0x0F, 0x51, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::SQRTSS_XMM_XMMM32);
}

// ============================================================================
// Interpreter Test Fixture
// ============================================================================

class SseArithTest : public ::testing::Test {
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
// 1. ADDSS: 1.0 + 2.0 = 3.0
// ============================================================================
TEST_F(SseArithTest, AddssBasic) {
    m_cpu->sse.xmm[0].f32_[0] = 1.0f;
    m_cpu->sse.xmm[0].f32_[1] = 99.0f;  // should be preserved
    m_cpu->sse.xmm[1].f32_[0] = 2.0f;

    u8 code[] = { 0xF3, 0x0F, 0x58, 0xC1 };  // ADDSS xmm0,xmm1
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[0], 3.0f);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[1], 99.0f);  // upper preserved
}

// ============================================================================
// 2. ADDSD: 1.0 + 2.5 = 3.5
// ============================================================================
TEST_F(SseArithTest, AddsdBasic) {
    m_cpu->sse.xmm[0].f64_[0] = 1.0;
    m_cpu->sse.xmm[1].f64_[0] = 2.5;

    u8 code[] = { 0xF2, 0x0F, 0x58, 0xC1 };  // ADDSD xmm0,xmm1
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_DOUBLE_EQ(m_cpu->sse.xmm[0].f64_[0], 3.5);
}

// ============================================================================
// 3. SUBSS: 5.0 - 2.0 = 3.0
// ============================================================================
TEST_F(SseArithTest, SubssBasic) {
    m_cpu->sse.xmm[0].f32_[0] = 5.0f;
    m_cpu->sse.xmm[1].f32_[0] = 2.0f;

    u8 code[] = { 0xF3, 0x0F, 0x5C, 0xC1 };  // SUBSS xmm0,xmm1
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[0], 3.0f);
}

// ============================================================================
// 4. MULSS: 3.0 * 4.0 = 12.0
// ============================================================================
TEST_F(SseArithTest, MulssBasic) {
    m_cpu->sse.xmm[0].f32_[0] = 3.0f;
    m_cpu->sse.xmm[1].f32_[0] = 4.0f;

    u8 code[] = { 0xF3, 0x0F, 0x59, 0xC1 };  // MULSS xmm0,xmm1
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[0], 12.0f);
}

// ============================================================================
// 5. DIVSS: 12.0 / 4.0 = 3.0
// ============================================================================
TEST_F(SseArithTest, DivssBasic) {
    m_cpu->sse.xmm[0].f32_[0] = 12.0f;
    m_cpu->sse.xmm[1].f32_[0] = 4.0f;

    u8 code[] = { 0xF3, 0x0F, 0x5E, 0xC1 };  // DIVSS xmm0,xmm1
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[0], 3.0f);
}

// ============================================================================
// 6. ADDPS packed: 4-lane addition
// ============================================================================
TEST_F(SseArithTest, AddpsPacked) {
    m_cpu->sse.xmm[0].f32_[0] = 1.0f;
    m_cpu->sse.xmm[0].f32_[1] = 2.0f;
    m_cpu->sse.xmm[0].f32_[2] = 3.0f;
    m_cpu->sse.xmm[0].f32_[3] = 4.0f;

    m_cpu->sse.xmm[1].f32_[0] = 10.0f;
    m_cpu->sse.xmm[1].f32_[1] = 20.0f;
    m_cpu->sse.xmm[1].f32_[2] = 30.0f;
    m_cpu->sse.xmm[1].f32_[3] = 40.0f;

    u8 code[] = { 0x0F, 0x58, 0xC1 };  // ADDPS xmm0,xmm1
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[0], 11.0f);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[1], 22.0f);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[2], 33.0f);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[3], 44.0f);
}

// ============================================================================
// 7. ADDPD packed: 2-lane addition
// ============================================================================
TEST_F(SseArithTest, AddpdPacked) {
    m_cpu->sse.xmm[0].f64_[0] = 1.5;
    m_cpu->sse.xmm[0].f64_[1] = 2.5;

    m_cpu->sse.xmm[1].f64_[0] = 10.0;
    m_cpu->sse.xmm[1].f64_[1] = 20.0;

    u8 code[] = { 0x66, 0x0F, 0x58, 0xC1 };  // ADDPD xmm0,xmm1
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_DOUBLE_EQ(m_cpu->sse.xmm[0].f64_[0], 11.5);
    EXPECT_DOUBLE_EQ(m_cpu->sse.xmm[0].f64_[1], 22.5);
}

// ============================================================================
// 8. SQRTSS: sqrt(9.0) = 3.0
// ============================================================================
TEST_F(SseArithTest, SqrtssBasic) {
    m_cpu->sse.xmm[1].f32_[0] = 9.0f;

    u8 code[] = { 0xF3, 0x0F, 0x51, 0xC1 };  // SQRTSS xmm0,xmm1
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[0], 3.0f);
}

// ============================================================================
// 9. COMISS sets flags: less, equal, greater
// ============================================================================
TEST_F(SseArithTest, ComissGreater) {
    m_cpu->sse.xmm[0].f32_[0] = 5.0f;
    m_cpu->sse.xmm[1].f32_[0] = 2.0f;

    u8 code[] = { 0x0F, 0x2F, 0xC1 };  // COMISS xmm0,xmm1
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_FALSE(m_cpu->flags.zf());
    EXPECT_FALSE(m_cpu->flags.pf());
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.of());
    EXPECT_FALSE(m_cpu->flags.sf());
}

TEST_F(SseArithTest, ComissLess) {
    m_cpu->sse.xmm[0].f32_[0] = 1.0f;
    m_cpu->sse.xmm[1].f32_[0] = 5.0f;

    u8 code[] = { 0x0F, 0x2F, 0xC1 };  // COMISS xmm0,xmm1
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_FALSE(m_cpu->flags.zf());
    EXPECT_FALSE(m_cpu->flags.pf());
    EXPECT_TRUE(m_cpu->flags.cf());
}

TEST_F(SseArithTest, ComissEqual) {
    m_cpu->sse.xmm[0].f32_[0] = 3.0f;
    m_cpu->sse.xmm[1].f32_[0] = 3.0f;

    u8 code[] = { 0x0F, 0x2F, 0xC1 };  // COMISS xmm0,xmm1
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_TRUE(m_cpu->flags.zf());
    EXPECT_FALSE(m_cpu->flags.pf());
    EXPECT_FALSE(m_cpu->flags.cf());
}

// ============================================================================
// 10. COMISS with NaN sets ZF=PF=CF=1
// ============================================================================
TEST_F(SseArithTest, ComissNaN) {
    m_cpu->sse.xmm[0].f32_[0] = std::numeric_limits<f32>::quiet_NaN();
    m_cpu->sse.xmm[1].f32_[0] = 1.0f;

    u8 code[] = { 0x0F, 0x2F, 0xC1 };  // COMISS xmm0,xmm1
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_TRUE(m_cpu->flags.zf());
    EXPECT_TRUE(m_cpu->flags.pf());
    EXPECT_TRUE(m_cpu->flags.cf());
    EXPECT_FALSE(m_cpu->flags.of());
    EXPECT_FALSE(m_cpu->flags.sf());
}

// ============================================================================
// 11. MAXSS / MINSS
// ============================================================================
TEST_F(SseArithTest, MaxssSelectsLarger) {
    m_cpu->sse.xmm[0].f32_[0] = 3.0f;
    m_cpu->sse.xmm[1].f32_[0] = 7.0f;

    u8 code[] = { 0xF3, 0x0F, 0x5F, 0xC1 };  // MAXSS xmm0,xmm1
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[0], 7.0f);
}

TEST_F(SseArithTest, MinssSelectsSmaller) {
    m_cpu->sse.xmm[0].f32_[0] = 3.0f;
    m_cpu->sse.xmm[1].f32_[0] = 7.0f;

    u8 code[] = { 0xF3, 0x0F, 0x5D, 0xC1 };  // MINSS xmm0,xmm1
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[0], 3.0f);
}

// ============================================================================
// 12. RCPSS approximate: 1.0 / 4.0 ≈ 0.25
// ============================================================================
TEST_F(SseArithTest, RcpssApproximate) {
    m_cpu->sse.xmm[1].f32_[0] = 4.0f;

    u8 code[] = { 0xF3, 0x0F, 0x53, 0xC1 };  // RCPSS xmm0,xmm1
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_NEAR(m_cpu->sse.xmm[0].f32_[0], 0.25f, 0.01f);
}

// ============================================================================
// 13. RSQRTSS approximate: 1/sqrt(4.0) ≈ 0.5
// ============================================================================
TEST_F(SseArithTest, RsqrtssApproximate) {
    m_cpu->sse.xmm[1].f32_[0] = 4.0f;

    u8 code[] = { 0xF3, 0x0F, 0x52, 0xC1 };  // RSQRTSS xmm0,xmm1
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_NEAR(m_cpu->sse.xmm[0].f32_[0], 0.5f, 0.01f);
}

// ============================================================================
// 14. Scalar preserves upper elements
// ============================================================================
TEST_F(SseArithTest, AddssPreservesUpperLanes) {
    m_cpu->sse.xmm[0].f32_[0] = 1.0f;
    m_cpu->sse.xmm[0].f32_[1] = 10.0f;
    m_cpu->sse.xmm[0].f32_[2] = 20.0f;
    m_cpu->sse.xmm[0].f32_[3] = 30.0f;
    m_cpu->sse.xmm[1].f32_[0] = 5.0f;

    u8 code[] = { 0xF3, 0x0F, 0x58, 0xC1 };  // ADDSS xmm0,xmm1
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[0], 6.0f);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[1], 10.0f);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[2], 20.0f);
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[3], 30.0f);
}
