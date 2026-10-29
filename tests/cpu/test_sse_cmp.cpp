// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// SSE comparison instruction tests (CPU-036)

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

TEST(SseCmpDecoder, CmppsXmm0Xmm1Eq) {
    // 0F C2 C1 00 — CMPPS xmm0, xmm1, 0 (EQ)
    auto inst = decode({0x0F, 0xC2, 0xC1, 0x00});
    EXPECT_EQ(inst.opcode, Opcode::CMPPS_XMM_XMMM128_IMM8);
    EXPECT_EQ(inst.operand_count, 3);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg128);
    EXPECT_EQ(inst.operands[0].reg, 0);
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg128);
    EXPECT_EQ(inst.operands[1].reg, 1);
    EXPECT_EQ(inst.operands[2].type, OperandType::Imm8);
    EXPECT_EQ(inst.operands[2].imm, 0);
}

TEST(SseCmpDecoder, CmpssXmm0Xmm1Lt) {
    // F3 0F C2 C1 01 — CMPSS xmm0, xmm1, 1 (LT)
    auto inst = decode({0xF3, 0x0F, 0xC2, 0xC1, 0x01});
    EXPECT_EQ(inst.opcode, Opcode::CMPSS_XMM_XMMM32_IMM8);
    EXPECT_EQ(inst.operand_count, 3);
    EXPECT_EQ(inst.operands[2].imm, 1);
}

TEST(SseCmpDecoder, CmppdXmm0Xmm1Le) {
    // 66 0F C2 C1 02 — CMPPD xmm0, xmm1, 2 (LE)
    auto inst = decode({0x66, 0x0F, 0xC2, 0xC1, 0x02});
    EXPECT_EQ(inst.opcode, Opcode::CMPPD_XMM_XMMM128_IMM8);
    EXPECT_EQ(inst.operands[2].imm, 2);
}

TEST(SseCmpDecoder, CmpsdXmm0Xmm1Neq) {
    // F2 0F C2 C1 04 — CMPSD xmm0, xmm1, 4 (NEQ)
    auto inst = decode({0xF2, 0x0F, 0xC2, 0xC1, 0x04});
    EXPECT_EQ(inst.opcode, Opcode::CMPSD_XMM_XMMM64_IMM8);
    EXPECT_EQ(inst.operands[2].imm, 4);
}

TEST(SseCmpDecoder, PcmpeqbXmm0Xmm1) {
    // 66 0F 74 C1 — PCMPEQB xmm0, xmm1
    auto inst = decode({0x66, 0x0F, 0x74, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::PCMPEQB_XMM_XMMM128);
    EXPECT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg128);
    EXPECT_EQ(inst.operands[0].reg, 0);
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg128);
    EXPECT_EQ(inst.operands[1].reg, 1);
}

TEST(SseCmpDecoder, PcmpeqdXmm0Xmm1) {
    // 66 0F 76 C1 — PCMPEQD xmm0, xmm1
    auto inst = decode({0x66, 0x0F, 0x76, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::PCMPEQD_XMM_XMMM128);
}

TEST(SseCmpDecoder, PcmpgtbXmm0Xmm1) {
    // 66 0F 64 C1 — PCMPGTB xmm0, xmm1
    auto inst = decode({0x66, 0x0F, 0x64, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::PCMPGTB_XMM_XMMM128);
}

TEST(SseCmpDecoder, PcmpgtdXmm0Xmm1) {
    // 66 0F 66 C1 — PCMPGTD xmm0, xmm1
    auto inst = decode({0x66, 0x0F, 0x66, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::PCMPGTD_XMM_XMMM128);
}

// ============================================================================
// Interpreter Test Fixture
// ============================================================================

class SseCmpTest : public ::testing::Test {
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
// 1. CMPPS EQ: equal elements → all-ones mask
// ============================================================================
TEST_F(SseCmpTest, CmppsEqAllEqual) {
    m_cpu->sse.xmm[0].f32_[0] = 1.0f;
    m_cpu->sse.xmm[0].f32_[1] = 2.0f;
    m_cpu->sse.xmm[0].f32_[2] = 3.0f;
    m_cpu->sse.xmm[0].f32_[3] = 4.0f;

    m_cpu->sse.xmm[1].f32_[0] = 1.0f;
    m_cpu->sse.xmm[1].f32_[1] = 2.0f;
    m_cpu->sse.xmm[1].f32_[2] = 3.0f;
    m_cpu->sse.xmm[1].f32_[3] = 4.0f;

    // 0F C2 C1 00 — CMPPS xmm0, xmm1, 0 (EQ)
    u8 code[] = { 0x0F, 0xC2, 0xC1, 0x00 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[0], 0xFFFFFFFFu);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[1], 0xFFFFFFFFu);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[2], 0xFFFFFFFFu);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[3], 0xFFFFFFFFu);
}

// ============================================================================
// 2. CMPPS EQ: unequal → all-zeros
// ============================================================================
TEST_F(SseCmpTest, CmppsEqUnequal) {
    m_cpu->sse.xmm[0].f32_[0] = 1.0f;
    m_cpu->sse.xmm[0].f32_[1] = 2.0f;
    m_cpu->sse.xmm[0].f32_[2] = 3.0f;
    m_cpu->sse.xmm[0].f32_[3] = 4.0f;

    m_cpu->sse.xmm[1].f32_[0] = 5.0f;
    m_cpu->sse.xmm[1].f32_[1] = 6.0f;
    m_cpu->sse.xmm[1].f32_[2] = 7.0f;
    m_cpu->sse.xmm[1].f32_[3] = 8.0f;

    u8 code[] = { 0x0F, 0xC2, 0xC1, 0x00 };  // CMPPS EQ
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[0], 0x00000000u);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[1], 0x00000000u);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[2], 0x00000000u);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[3], 0x00000000u);
}

// ============================================================================
// 3. CMPPS LT: less-than
// ============================================================================
TEST_F(SseCmpTest, CmppsLt) {
    m_cpu->sse.xmm[0].f32_[0] = 1.0f;  // < 5.0 → true
    m_cpu->sse.xmm[0].f32_[1] = 6.0f;  // < 2.0 → false
    m_cpu->sse.xmm[0].f32_[2] = 3.0f;  // < 3.0 → false (equal)
    m_cpu->sse.xmm[0].f32_[3] = 0.0f;  // < 9.0 → true

    m_cpu->sse.xmm[1].f32_[0] = 5.0f;
    m_cpu->sse.xmm[1].f32_[1] = 2.0f;
    m_cpu->sse.xmm[1].f32_[2] = 3.0f;
    m_cpu->sse.xmm[1].f32_[3] = 9.0f;

    u8 code[] = { 0x0F, 0xC2, 0xC1, 0x01 };  // CMPPS LT
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[0], 0xFFFFFFFFu);  // true
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[1], 0x00000000u);  // false
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[2], 0x00000000u);  // false
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[3], 0xFFFFFFFFu);  // true
}

// ============================================================================
// 4. CMPPS LE: less-equal
// ============================================================================
TEST_F(SseCmpTest, CmppsLe) {
    m_cpu->sse.xmm[0].f32_[0] = 1.0f;  // <= 5.0 → true
    m_cpu->sse.xmm[0].f32_[1] = 6.0f;  // <= 2.0 → false
    m_cpu->sse.xmm[0].f32_[2] = 3.0f;  // <= 3.0 → true (equal)
    m_cpu->sse.xmm[0].f32_[3] = 9.0f;  // <= 9.0 → true (equal)

    m_cpu->sse.xmm[1].f32_[0] = 5.0f;
    m_cpu->sse.xmm[1].f32_[1] = 2.0f;
    m_cpu->sse.xmm[1].f32_[2] = 3.0f;
    m_cpu->sse.xmm[1].f32_[3] = 9.0f;

    u8 code[] = { 0x0F, 0xC2, 0xC1, 0x02 };  // CMPPS LE
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[0], 0xFFFFFFFFu);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[1], 0x00000000u);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[2], 0xFFFFFFFFu);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[3], 0xFFFFFFFFu);
}

// ============================================================================
// 5. CMPPS UNORD: NaN → all-ones
// ============================================================================
TEST_F(SseCmpTest, CmppsUnord) {
    m_cpu->sse.xmm[0].f32_[0] = std::numeric_limits<f32>::quiet_NaN();
    m_cpu->sse.xmm[0].f32_[1] = 1.0f;
    m_cpu->sse.xmm[0].f32_[2] = 2.0f;
    m_cpu->sse.xmm[0].f32_[3] = std::numeric_limits<f32>::quiet_NaN();

    m_cpu->sse.xmm[1].f32_[0] = 1.0f;
    m_cpu->sse.xmm[1].f32_[1] = 2.0f;
    m_cpu->sse.xmm[1].f32_[2] = std::numeric_limits<f32>::quiet_NaN();
    m_cpu->sse.xmm[1].f32_[3] = std::numeric_limits<f32>::quiet_NaN();

    u8 code[] = { 0x0F, 0xC2, 0xC1, 0x03 };  // CMPPS UNORD
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[0], 0xFFFFFFFFu);  // NaN in dst
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[1], 0x00000000u);  // both ordered
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[2], 0xFFFFFFFFu);  // NaN in src
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[3], 0xFFFFFFFFu);  // NaN in both
}

// ============================================================================
// 6. CMPPS NEQ: not-equal
// ============================================================================
TEST_F(SseCmpTest, CmppsNeq) {
    m_cpu->sse.xmm[0].f32_[0] = 1.0f;
    m_cpu->sse.xmm[0].f32_[1] = 2.0f;
    m_cpu->sse.xmm[0].f32_[2] = 3.0f;
    m_cpu->sse.xmm[0].f32_[3] = 4.0f;

    m_cpu->sse.xmm[1].f32_[0] = 1.0f;  // equal → false
    m_cpu->sse.xmm[1].f32_[1] = 9.0f;  // not equal → true
    m_cpu->sse.xmm[1].f32_[2] = 3.0f;  // equal → false
    m_cpu->sse.xmm[1].f32_[3] = 9.0f;  // not equal → true

    u8 code[] = { 0x0F, 0xC2, 0xC1, 0x04 };  // CMPPS NEQ
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[0], 0x00000000u);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[1], 0xFFFFFFFFu);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[2], 0x00000000u);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[3], 0xFFFFFFFFu);
}

// ============================================================================
// 7. CMPPS ORD: non-NaN → all-ones
// ============================================================================
TEST_F(SseCmpTest, CmppsOrd) {
    m_cpu->sse.xmm[0].f32_[0] = 1.0f;
    m_cpu->sse.xmm[0].f32_[1] = std::numeric_limits<f32>::quiet_NaN();
    m_cpu->sse.xmm[0].f32_[2] = 3.0f;
    m_cpu->sse.xmm[0].f32_[3] = 4.0f;

    m_cpu->sse.xmm[1].f32_[0] = 2.0f;
    m_cpu->sse.xmm[1].f32_[1] = 5.0f;
    m_cpu->sse.xmm[1].f32_[2] = 6.0f;
    m_cpu->sse.xmm[1].f32_[3] = std::numeric_limits<f32>::quiet_NaN();

    u8 code[] = { 0x0F, 0xC2, 0xC1, 0x07 };  // CMPPS ORD
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[0], 0xFFFFFFFFu);  // both ordered
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[1], 0x00000000u);  // NaN in dst
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[2], 0xFFFFFFFFu);  // both ordered
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[3], 0x00000000u);  // NaN in src
}

// ============================================================================
// 8. CMPSS scalar: only low element compared, upper preserved
// ============================================================================
TEST_F(SseCmpTest, CmpssScalarPreservesUpper) {
    m_cpu->sse.xmm[0].f32_[0] = 5.0f;
    m_cpu->sse.xmm[0].f32_[1] = 10.0f;
    m_cpu->sse.xmm[0].f32_[2] = 20.0f;
    m_cpu->sse.xmm[0].f32_[3] = 30.0f;

    m_cpu->sse.xmm[1].f32_[0] = 5.0f;  // equal → all-ones in low element

    // F3 0F C2 C1 00 — CMPSS xmm0, xmm1, 0 (EQ)
    u8 code[] = { 0xF3, 0x0F, 0xC2, 0xC1, 0x00 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[0], 0xFFFFFFFFu);  // EQ → true
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[1], 10.0f);   // preserved
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[2], 20.0f);   // preserved
    EXPECT_FLOAT_EQ(m_cpu->sse.xmm[0].f32_[3], 30.0f);   // preserved
}

// ============================================================================
// 9. CMPPD: packed double comparison (EQ)
// ============================================================================
TEST_F(SseCmpTest, CmppdEq) {
    m_cpu->sse.xmm[0].f64_[0] = 1.5;
    m_cpu->sse.xmm[0].f64_[1] = 2.5;

    m_cpu->sse.xmm[1].f64_[0] = 1.5;  // equal
    m_cpu->sse.xmm[1].f64_[1] = 9.9;  // not equal

    // 66 0F C2 C1 00 — CMPPD xmm0, xmm1, 0 (EQ)
    u8 code[] = { 0x66, 0x0F, 0xC2, 0xC1, 0x00 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[0], 0xFFFFFFFFFFFFFFFFull);
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[1], 0x0000000000000000ull);
}

// ============================================================================
// 10. CMPSD scalar: low qword compared, upper preserved
// ============================================================================
TEST_F(SseCmpTest, CmpsdScalarPreservesUpper) {
    m_cpu->sse.xmm[0].f64_[0] = 3.14;
    m_cpu->sse.xmm[0].f64_[1] = 99.99;  // should be preserved

    m_cpu->sse.xmm[1].f64_[0] = 3.14;  // equal

    // F2 0F C2 C1 00 — CMPSD xmm0, xmm1, 0 (EQ)
    u8 code[] = { 0xF2, 0x0F, 0xC2, 0xC1, 0x00 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[0], 0xFFFFFFFFFFFFFFFFull);
    EXPECT_DOUBLE_EQ(m_cpu->sse.xmm[0].f64_[1], 99.99);  // upper preserved
}

// ============================================================================
// 11. PCMPEQB: byte equality mask
// ============================================================================
TEST_F(SseCmpTest, PcmpeqbByteMask) {
    // Set up: bytes 0,2,4 equal, others different
    std::memset(&m_cpu->sse.xmm[0], 0, 16);
    std::memset(&m_cpu->sse.xmm[1], 0, 16);
    for (int i = 0; i < 16; ++i) {
        m_cpu->sse.xmm[0].u8_[i] = static_cast<u8>(i);
        m_cpu->sse.xmm[1].u8_[i] = static_cast<u8>(i + 1);  // all different by 1
    }
    // Make bytes 0 and 5 equal
    m_cpu->sse.xmm[1].u8_[0] = 0;
    m_cpu->sse.xmm[1].u8_[5] = 5;

    // 66 0F 74 C1 — PCMPEQB xmm0, xmm1
    u8 code[] = { 0x66, 0x0F, 0x74, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u8_[0], 0xFF);   // equal
    EXPECT_EQ(m_cpu->sse.xmm[0].u8_[1], 0x00);   // not equal
    EXPECT_EQ(m_cpu->sse.xmm[0].u8_[2], 0x00);   // not equal
    EXPECT_EQ(m_cpu->sse.xmm[0].u8_[5], 0xFF);   // equal
    EXPECT_EQ(m_cpu->sse.xmm[0].u8_[15], 0x00);  // not equal
}

// ============================================================================
// 12. PCMPEQD: dword equality mask
// ============================================================================
TEST_F(SseCmpTest, PcmpeqdDwordMask) {
    m_cpu->sse.xmm[0].u32_[0] = 100;
    m_cpu->sse.xmm[0].u32_[1] = 200;
    m_cpu->sse.xmm[0].u32_[2] = 300;
    m_cpu->sse.xmm[0].u32_[3] = 400;

    m_cpu->sse.xmm[1].u32_[0] = 100;  // equal
    m_cpu->sse.xmm[1].u32_[1] = 999;  // not equal
    m_cpu->sse.xmm[1].u32_[2] = 300;  // equal
    m_cpu->sse.xmm[1].u32_[3] = 999;  // not equal

    // 66 0F 76 C1 — PCMPEQD xmm0, xmm1
    u8 code[] = { 0x66, 0x0F, 0x76, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[0], 0xFFFFFFFFu);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[1], 0x00000000u);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[2], 0xFFFFFFFFu);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[3], 0x00000000u);
}

// ============================================================================
// 13. PCMPGTB: signed byte greater-than
// ============================================================================
TEST_F(SseCmpTest, PcmpgtbSignedByte) {
    // dst[0]=5 > src[0]=3 → 0xFF
    // dst[1]=-1 > src[1]=1 → 0x00 (signed: -1 < 1)
    // dst[2]=0 > src[2]=0 → 0x00 (equal, not greater)
    // dst[3]=127 > src[3]=-128 → 0xFF
    std::memset(&m_cpu->sse.xmm[0], 0, 16);
    std::memset(&m_cpu->sse.xmm[1], 0, 16);
    m_cpu->sse.xmm[0].i8_[0] = 5;
    m_cpu->sse.xmm[0].i8_[1] = -1;
    m_cpu->sse.xmm[0].i8_[2] = 0;
    m_cpu->sse.xmm[0].i8_[3] = 127;

    m_cpu->sse.xmm[1].i8_[0] = 3;
    m_cpu->sse.xmm[1].i8_[1] = 1;
    m_cpu->sse.xmm[1].i8_[2] = 0;
    m_cpu->sse.xmm[1].i8_[3] = -128;

    // 66 0F 64 C1 — PCMPGTB xmm0, xmm1
    u8 code[] = { 0x66, 0x0F, 0x64, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u8_[0], 0xFF);  // 5 > 3
    EXPECT_EQ(m_cpu->sse.xmm[0].u8_[1], 0x00);  // -1 not > 1
    EXPECT_EQ(m_cpu->sse.xmm[0].u8_[2], 0x00);  // 0 not > 0
    EXPECT_EQ(m_cpu->sse.xmm[0].u8_[3], 0xFF);  // 127 > -128
}

// ============================================================================
// 14. PCMPGTD: signed dword greater-than
// ============================================================================
TEST_F(SseCmpTest, PcmpgtdSignedDword) {
    m_cpu->sse.xmm[0].i32_[0] = 100;
    m_cpu->sse.xmm[0].i32_[1] = -50;
    m_cpu->sse.xmm[0].i32_[2] = 0;
    m_cpu->sse.xmm[0].i32_[3] = 2147483647;  // INT32_MAX

    m_cpu->sse.xmm[1].i32_[0] = 50;          // 100 > 50 → true
    m_cpu->sse.xmm[1].i32_[1] = -10;         // -50 > -10 → false
    m_cpu->sse.xmm[1].i32_[2] = 0;           // 0 > 0 → false
    m_cpu->sse.xmm[1].i32_[3] = -2147483648; // MAX > MIN → true

    // 66 0F 66 C1 — PCMPGTD xmm0, xmm1
    u8 code[] = { 0x66, 0x0F, 0x66, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[0], 0xFFFFFFFFu);  // true
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[1], 0x00000000u);  // false
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[2], 0x00000000u);  // false
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[3], 0xFFFFFFFFu);  // true
}
