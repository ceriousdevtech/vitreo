// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// SSE2 integer arithmetic instruction tests (CPU-037)

#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::cpu;

// ============================================================================
// Decode-level helper
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
// Interpreter Test Fixture
// ============================================================================

class SseIntTest : public ::testing::Test {
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
// 1. PADDB wrapping (255+1=0)
// ============================================================================
TEST_F(SseIntTest, PaddbWrapping) {
    auto& dst = m_cpu->sse.xmm[0];
    auto& src = m_cpu->sse.xmm[1];
    std::memset(&dst, 0, sizeof(dst));
    std::memset(&src, 0, sizeof(src));

    dst.u8_[0] = 255;
    dst.u8_[1] = 100;
    src.u8_[0] = 1;
    src.u8_[1] = 50;

    // 66 0F FC C1 — PADDB xmm0, xmm1
    u8 code[] = { 0x66, 0x0F, 0xFC, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u8_[0], 0);    // 255+1 wraps to 0
    EXPECT_EQ(m_cpu->sse.xmm[0].u8_[1], 150);   // 100+50
}

// ============================================================================
// 2. PADDW adds words
// ============================================================================
TEST_F(SseIntTest, PaddwBasic) {
    auto& dst = m_cpu->sse.xmm[0];
    auto& src = m_cpu->sse.xmm[1];
    std::memset(&dst, 0, sizeof(dst));
    std::memset(&src, 0, sizeof(src));

    dst.u16_[0] = 1000;
    dst.u16_[3] = 5000;
    src.u16_[0] = 2000;
    src.u16_[3] = 3000;

    // 66 0F FD C1 — PADDW xmm0, xmm1
    u8 code[] = { 0x66, 0x0F, 0xFD, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u16_[0], 3000);
    EXPECT_EQ(m_cpu->sse.xmm[0].u16_[3], 8000);
}

// ============================================================================
// 3. PADDD adds dwords
// ============================================================================
TEST_F(SseIntTest, PadddBasic) {
    auto& dst = m_cpu->sse.xmm[0];
    auto& src = m_cpu->sse.xmm[1];
    std::memset(&dst, 0, sizeof(dst));
    std::memset(&src, 0, sizeof(src));

    dst.u32_[0] = 100000;
    dst.u32_[2] = 200000;
    src.u32_[0] = 50000;
    src.u32_[2] = 300000;

    // 66 0F FE C1 — PADDD xmm0, xmm1
    u8 code[] = { 0x66, 0x0F, 0xFE, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[0], 150000u);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[2], 500000u);
}

// ============================================================================
// 4. PADDQ adds qwords
// ============================================================================
TEST_F(SseIntTest, PaddqBasic) {
    auto& dst = m_cpu->sse.xmm[0];
    auto& src = m_cpu->sse.xmm[1];
    std::memset(&dst, 0, sizeof(dst));
    std::memset(&src, 0, sizeof(src));

    dst.u64_[0] = 0x100000000ULL;
    dst.u64_[1] = 42;
    src.u64_[0] = 0x200000000ULL;
    src.u64_[1] = 58;

    // 66 0F D4 C1 — PADDQ xmm0, xmm1
    u8 code[] = { 0x66, 0x0F, 0xD4, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[0], 0x300000000ULL);
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[1], 100ULL);
}

// ============================================================================
// 5. PSUBB subtracts bytes
// ============================================================================
TEST_F(SseIntTest, PsubbBasic) {
    auto& dst = m_cpu->sse.xmm[0];
    auto& src = m_cpu->sse.xmm[1];
    std::memset(&dst, 0, sizeof(dst));
    std::memset(&src, 0, sizeof(src));

    dst.u8_[0] = 100;
    dst.u8_[1] = 0;    // wraps: 0 - 1 = 255
    src.u8_[0] = 30;
    src.u8_[1] = 1;

    // 66 0F F8 C1 — PSUBB xmm0, xmm1
    u8 code[] = { 0x66, 0x0F, 0xF8, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u8_[0], 70);
    EXPECT_EQ(m_cpu->sse.xmm[0].u8_[1], 255);   // wrapping underflow
}

// ============================================================================
// 6. PADDUSB saturates at 255
// ============================================================================
TEST_F(SseIntTest, PaddusbSaturates) {
    auto& dst = m_cpu->sse.xmm[0];
    auto& src = m_cpu->sse.xmm[1];
    std::memset(&dst, 0, sizeof(dst));
    std::memset(&src, 0, sizeof(src));

    dst.u8_[0] = 200;
    dst.u8_[1] = 100;
    src.u8_[0] = 100;   // 200+100=300 → clamped to 255
    src.u8_[1] = 50;    // 100+50=150

    // 66 0F DC C1 — PADDUSB xmm0, xmm1
    u8 code[] = { 0x66, 0x0F, 0xDC, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u8_[0], 255);
    EXPECT_EQ(m_cpu->sse.xmm[0].u8_[1], 150);
}

// ============================================================================
// 7. PSUBUSB saturates at 0
// ============================================================================
TEST_F(SseIntTest, PsubusbSaturates) {
    auto& dst = m_cpu->sse.xmm[0];
    auto& src = m_cpu->sse.xmm[1];
    std::memset(&dst, 0, sizeof(dst));
    std::memset(&src, 0, sizeof(src));

    dst.u8_[0] = 10;
    dst.u8_[1] = 200;
    src.u8_[0] = 50;    // 10-50 → clamped to 0
    src.u8_[1] = 100;   // 200-100=100

    // 66 0F D8 C1 — PSUBUSB xmm0, xmm1
    u8 code[] = { 0x66, 0x0F, 0xD8, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u8_[0], 0);
    EXPECT_EQ(m_cpu->sse.xmm[0].u8_[1], 100);
}

// ============================================================================
// 8. PMULLW multiplies (low 16 bits)
// ============================================================================
TEST_F(SseIntTest, PmullwLow) {
    auto& dst = m_cpu->sse.xmm[0];
    auto& src = m_cpu->sse.xmm[1];
    std::memset(&dst, 0, sizeof(dst));
    std::memset(&src, 0, sizeof(src));

    dst.i16_[0] = 100;
    dst.i16_[1] = -5;
    src.i16_[0] = 200;
    src.i16_[1] = 3;

    // 66 0F D5 C1 — PMULLW xmm0, xmm1
    u8 code[] = { 0x66, 0x0F, 0xD5, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // 100*200 = 20000, low 16 bits = 20000
    EXPECT_EQ(m_cpu->sse.xmm[0].i16_[0], static_cast<i16>(20000 & 0xFFFF));
    // -5*3 = -15, low 16 bits = -15 (0xFFF1)
    EXPECT_EQ(m_cpu->sse.xmm[0].i16_[1], -15);
}

// ============================================================================
// 9. PMULHW multiplies (high signed 16 bits)
// ============================================================================
TEST_F(SseIntTest, PmulhwHigh) {
    auto& dst = m_cpu->sse.xmm[0];
    auto& src = m_cpu->sse.xmm[1];
    std::memset(&dst, 0, sizeof(dst));
    std::memset(&src, 0, sizeof(src));

    // 0x4000 * 0x4000 = 0x10000000 → high 16 bits = 0x1000
    dst.i16_[0] = 0x4000;
    src.i16_[0] = 0x4000;
    // small values: 2*3=6 → high 16 bits = 0
    dst.i16_[1] = 2;
    src.i16_[1] = 3;

    // 66 0F E5 C1 — PMULHW xmm0, xmm1
    u8 code[] = { 0x66, 0x0F, 0xE5, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].i16_[0], 0x1000);
    EXPECT_EQ(m_cpu->sse.xmm[0].i16_[1], 0);
}

// ============================================================================
// 10. PMADDWD multiply-accumulate
// ============================================================================
TEST_F(SseIntTest, PmaddwdBasic) {
    auto& dst = m_cpu->sse.xmm[0];
    auto& src = m_cpu->sse.xmm[1];
    std::memset(&dst, 0, sizeof(dst));
    std::memset(&src, 0, sizeof(src));

    // result.i32_[0] = dst.i16_[0]*src.i16_[0] + dst.i16_[1]*src.i16_[1]
    //                = 3*4 + 5*6 = 12 + 30 = 42
    dst.i16_[0] = 3;
    dst.i16_[1] = 5;
    src.i16_[0] = 4;
    src.i16_[1] = 6;

    // result.i32_[1] = dst.i16_[2]*src.i16_[2] + dst.i16_[3]*src.i16_[3]
    //                = 10*(-2) + 7*3 = -20 + 21 = 1
    dst.i16_[2] = 10;
    dst.i16_[3] = 7;
    src.i16_[2] = -2;
    src.i16_[3] = 3;

    // 66 0F F5 C1 — PMADDWD xmm0, xmm1
    u8 code[] = { 0x66, 0x0F, 0xF5, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].i32_[0], 42);
    EXPECT_EQ(m_cpu->sse.xmm[0].i32_[1], 1);
}

// ============================================================================
// 10b. PMULUDQ basic — unsigned dword multiply → qword
// ============================================================================
TEST_F(SseIntTest, PmuludqBasic) {
    auto& dst = m_cpu->sse.xmm[0];
    auto& src = m_cpu->sse.xmm[1];
    std::memset(&dst, 0, sizeof(dst));
    std::memset(&src, 0, sizeof(src));

    // dst.u32[0] * src.u32[0] → result.u64[0]
    // dst.u32[2] * src.u32[2] → result.u64[1]
    dst.u32_[0] = 1000;
    dst.u32_[2] = 500;
    src.u32_[0] = 2000;
    src.u32_[2] = 300;

    // 66 0F F4 C1 — PMULUDQ xmm0, xmm1
    u8 code[] = { 0x66, 0x0F, 0xF4, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[0], 2000000ULL); // 1000 * 2000
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[1], 150000ULL);  // 500 * 300
}

// ============================================================================
// 10c. PMULUDQ large values — Ed25519 uses large 32-bit multiplies
// ============================================================================
TEST_F(SseIntTest, PmuludqLargeValues) {
    auto& dst = m_cpu->sse.xmm[0];
    auto& src = m_cpu->sse.xmm[1];
    std::memset(&dst, 0, sizeof(dst));
    std::memset(&src, 0, sizeof(src));

    // Max unsigned 32-bit: 0xFFFFFFFF * 0xFFFFFFFF = 0xFFFFFFFE00000001
    dst.u32_[0] = 0xFFFFFFFF;
    src.u32_[0] = 0xFFFFFFFF;
    // Also test with moderate large values
    dst.u32_[2] = 0x7FFFFFFF;  // 2^31 - 1
    src.u32_[2] = 0x80000000;  // 2^31

    u8 code[] = { 0x66, 0x0F, 0xF4, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[0], 0xFFFFFFFE00000001ULL);
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[1], 0x3FFFFFFF80000000ULL); // (2^31-1) * 2^31
}

// ============================================================================
// 10d. PMULUDQ with 26-bit values (Ed25519 uses radix-2^26 representation)
// ============================================================================
TEST_F(SseIntTest, PmuludqRadix26) {
    auto& dst = m_cpu->sse.xmm[0];
    auto& src = m_cpu->sse.xmm[1];
    std::memset(&dst, 0, sizeof(dst));
    std::memset(&src, 0, sizeof(src));

    // Ed25519 uses values up to 2^26 - 1 = 0x3FFFFFF
    dst.u32_[0] = 0x3FFFFFF;  // max limb
    src.u32_[0] = 0x3FFFFFF;
    dst.u32_[2] = 0x1FFFFFF;  // 2^25 - 1 (alternate limb size)
    src.u32_[2] = 0x1FFFFFF;

    u8 code[] = { 0x66, 0x0F, 0xF4, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // 0x3FFFFFF * 0x3FFFFFF = 0xFFFFFF8000001
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[0], 0xFFFFFF8000001ULL);
    // 0x1FFFFFF * 0x1FFFFFF = 0x3FFFFFC000001
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[1], 0x3FFFFFC000001ULL);
}

// ============================================================================
// 11. PMINUB selects min bytes
// ============================================================================
TEST_F(SseIntTest, PminubSelectsMin) {
    auto& dst = m_cpu->sse.xmm[0];
    auto& src = m_cpu->sse.xmm[1];
    std::memset(&dst, 0, sizeof(dst));
    std::memset(&src, 0, sizeof(src));

    dst.u8_[0] = 50;
    dst.u8_[1] = 200;
    src.u8_[0] = 100;
    src.u8_[1] = 150;

    // 66 0F DA C1 — PMINUB xmm0, xmm1
    u8 code[] = { 0x66, 0x0F, 0xDA, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u8_[0], 50);
    EXPECT_EQ(m_cpu->sse.xmm[0].u8_[1], 150);
}

// ============================================================================
// 12. PMAXUB selects max bytes
// ============================================================================
TEST_F(SseIntTest, PmaxubSelectsMax) {
    auto& dst = m_cpu->sse.xmm[0];
    auto& src = m_cpu->sse.xmm[1];
    std::memset(&dst, 0, sizeof(dst));
    std::memset(&src, 0, sizeof(src));

    dst.u8_[0] = 50;
    dst.u8_[1] = 200;
    src.u8_[0] = 100;
    src.u8_[1] = 150;

    // 66 0F DE C1 — PMAXUB xmm0, xmm1
    u8 code[] = { 0x66, 0x0F, 0xDE, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u8_[0], 100);
    EXPECT_EQ(m_cpu->sse.xmm[0].u8_[1], 200);
}

// ============================================================================
// 13. PAVGB averages with rounding
// ============================================================================
TEST_F(SseIntTest, PavgbRounding) {
    auto& dst = m_cpu->sse.xmm[0];
    auto& src = m_cpu->sse.xmm[1];
    std::memset(&dst, 0, sizeof(dst));
    std::memset(&src, 0, sizeof(src));

    // (10 + 20 + 1) >> 1 = 15 (rounds down, 31/2=15)
    dst.u8_[0] = 10;
    src.u8_[0] = 20;
    // (11 + 20 + 1) >> 1 = 16 (rounds up, 32/2=16)
    dst.u8_[1] = 11;
    src.u8_[1] = 20;

    // 66 0F E0 C1 — PAVGB xmm0, xmm1
    u8 code[] = { 0x66, 0x0F, 0xE0, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u8_[0], 15);
    EXPECT_EQ(m_cpu->sse.xmm[0].u8_[1], 16);
}

// ============================================================================
// 14. PSADBW computes SAD
// ============================================================================
TEST_F(SseIntTest, PsadbwBasic) {
    auto& dst = m_cpu->sse.xmm[0];
    auto& src = m_cpu->sse.xmm[1];
    std::memset(&dst, 0, sizeof(dst));
    std::memset(&src, 0, sizeof(src));

    // Low 8 bytes: each byte differs by 10 → SAD = 8*10 = 80
    for (int i = 0; i < 8; ++i) {
        dst.u8_[i] = 100;
        src.u8_[i] = 90;
    }
    // High 8 bytes: each byte differs by 5 → SAD = 8*5 = 40
    for (int i = 8; i < 16; ++i) {
        dst.u8_[i] = 50;
        src.u8_[i] = 45;
    }

    // 66 0F F6 C1 — PSADBW xmm0, xmm1
    u8 code[] = { 0x66, 0x0F, 0xF6, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[0], 80u);
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[1], 40u);
}

// ============================================================================
// 14b. Vectorized byte count (PCMPEQB + PSUBB + PSADBW)
// ----------------------------------------------------------------------------
// Compilers lower `std::count(p, p + n, c)` over bytes to this exact sequence:
// compare a 16-byte chunk against a broadcast needle, subtract the all-ones
// match mask into a byte accumulator, then fold the accumulator with PSADBW.
// The three instructions therefore have to agree as a unit, not just
// individually: an unsigned-vs-signed slip in any one of them silently
// miscounts, and callers that also count the same bytes with a scalar loop
// then disagree with themselves.
// ============================================================================
TEST_F(SseIntTest, VectorizedByteCountMatchesScalarCount) {
    constexpr u8 kNeedle = '_';
    constexpr GuestAddr kData = 0x2000;

    // MOVDQU xmm0, [rax] / PCMPEQB xmm0, xmm1 / PSUBB xmm2, xmm0 /
    // PSADBW xmm2, xmm3
    const u8 code[] = {
        0xF3, 0x0F, 0x6F, 0x00,       // movdqu xmm0, [rax]
        0x66, 0x0F, 0x74, 0xC1,       // pcmpeqb xmm0, xmm1
        0x66, 0x0F, 0xF8, 0xD0,       // psubb xmm2, xmm0
        0x66, 0x0F, 0xF6, 0xD3,       // psadbw xmm2, xmm3
    };

    // Byte patterns chosen to cover the classic failure modes: no match, all
    // match, matches only in one 64-bit half (PSADBW folds each half apart),
    // and bytes with bit 7 set, which distinguish an unsigned compare from a
    // signed one.
    const std::vector<std::string> inputs = {
        std::string(16, 'a'),
        std::string(16, '_'),
        "a_b_c_d_e_f_g_h_",
        "________abcdefgh",
        "abcdefgh________",
        "spotify_track_id",
        std::string("\x80\xFF_\x7F\x00_zz\xC0_aa\xFF__z", 16),
    };

    for (const std::string& input : inputs) {
        ASSERT_EQ(input.size(), 16u);

        auto write = m_memory->write_bytes(
            kData, reinterpret_cast<const u8*>(input.data()), input.size());
        ASSERT_TRUE(write);

        m_cpu->rip = 0x1000;
        m_cpu->gpr.set64(static_cast<u8>(RegIndex::RAX), kData);
        std::memset(&m_cpu->sse.xmm[0], 0, sizeof(m_cpu->sse.xmm[0]));
        std::memset(&m_cpu->sse.xmm[2], 0, sizeof(m_cpu->sse.xmm[2]));
        std::memset(&m_cpu->sse.xmm[3], 0, sizeof(m_cpu->sse.xmm[3]));
        for (int i = 0; i < 16; ++i) m_cpu->sse.xmm[1].u8_[i] = kNeedle;

        write_code(code, sizeof(code));
        for (int step = 0; step < 4; ++step) {
            ASSERT_EQ(execute_one(), ExecResult::Continue)
                << "step " << step << " of \"" << input << "\"";
        }

        const u64 counted =
            m_cpu->sse.xmm[2].u64_[0] + m_cpu->sse.xmm[2].u64_[1];
        const u64 expected = static_cast<u64>(
            std::count(input.begin(), input.end(), static_cast<char>(kNeedle)));
        EXPECT_EQ(counted, expected) << "vectorized count disagrees with the "
                                        "scalar count for \"" << input << "\"";
    }
}

// ============================================================================
// 14c. Vectorized byte count via PMOVMSKB + POPCNT
// ----------------------------------------------------------------------------
// The other lowering compilers pick for the same `std::count` over bytes:
// compare, extract one bit per byte, then population-count the mask. PMOVMSKB
// takes the *sign* bit of each byte and zero-extends the 16-bit result into a
// 32-bit register, so a byte with bit 7 set in the source data must not leak
// into the mask on its own — only the compare result may.
// ============================================================================
TEST_F(SseIntTest, ByteMaskPopcountMatchesScalarCount) {
    constexpr u8 kNeedle = '_';
    constexpr GuestAddr kData = 0x2000;

    const u8 code[] = {
        0xF3, 0x0F, 0x6F, 0x00,       // movdqu xmm0, [rax]
        0x66, 0x0F, 0x74, 0xC1,       // pcmpeqb xmm0, xmm1
        0x66, 0x0F, 0xD7, 0xC0,       // pmovmskb eax, xmm0
        0xF3, 0x0F, 0xB8, 0xC0,       // popcnt eax, eax
    };

    const std::vector<std::string> inputs = {
        std::string(16, 'a'),
        std::string(16, '_'),
        "a_b_c_d_e_f_g_h_",
        "spotify_track_id",
        std::string("\x80\xFF_\x7F\x00_zz\xC0_aa\xFF__z", 16),
        std::string("\x80\x80\x80\x80\x80\x80\x80\x80"
                    "\x80\x80\x80\x80\x80\x80\x80\x80", 16),
    };

    for (const std::string& input : inputs) {
        ASSERT_EQ(input.size(), 16u);

        auto write = m_memory->write_bytes(
            kData, reinterpret_cast<const u8*>(input.data()), input.size());
        ASSERT_TRUE(write);

        m_cpu->rip = 0x1000;
        m_cpu->gpr.set64(static_cast<u8>(RegIndex::RAX), kData);
        std::memset(&m_cpu->sse.xmm[0], 0, sizeof(m_cpu->sse.xmm[0]));
        for (int i = 0; i < 16; ++i) m_cpu->sse.xmm[1].u8_[i] = kNeedle;

        write_code(code, sizeof(code));
        for (int step = 0; step < 4; ++step) {
            ASSERT_EQ(execute_one(), ExecResult::Continue)
                << "step " << step << " of \"" << input << "\"";
        }

        const u64 counted = m_cpu->gpr.get32(0);
        const u64 expected = static_cast<u64>(
            std::count(input.begin(), input.end(), static_cast<char>(kNeedle)));
        EXPECT_EQ(counted, expected) << "mask/popcount count disagrees with the "
                                        "scalar count for \"" << input << "\"";
    }
}

// ============================================================================
// 14d. The CRT's vectorized byte count, instruction for instruction
// ----------------------------------------------------------------------------
// This is the exact body the Microsoft STL runs for `std::count` over bytes on
// an SSE4.2 machine: broadcast the needle with PSHUFB against a zeroed control
// mask, accumulate compare masks with PSUBB, fold with PSADBW, then read the
// two 64-bit halves back with MOVQ and PEXTRQ. It only runs for inputs of 16
// bytes or more; shorter inputs take a scalar tail. Callers pair this with a
// separate scalar loop over the same bytes and require both to agree, so every
// instruction in the chain has to be right together.
// ============================================================================
TEST_F(SseIntTest, CrtVectorizedByteCountMatchesScalarCount) {
    constexpr u8 kNeedle = '_';
    constexpr GuestAddr kData = 0x2000;

    const u8 code[] = {
        0x0F, 0x57, 0xDB,                   // xorps  xmm3, xmm3
        0x66, 0x0F, 0x6E, 0xD7,             // movd   xmm2, edi
        0x66, 0x0F, 0x38, 0x00, 0xD3,       // pshufb xmm2, xmm3
        0x66, 0x0F, 0x6F, 0xCB,             // movdqa xmm1, xmm3
        0xF3, 0x0F, 0x6F, 0x01,             // movdqu xmm0, [rcx]
        0x66, 0x0F, 0x74, 0xC2,             // pcmpeqb xmm0, xmm2
        0x66, 0x0F, 0xF8, 0xC8,             // psubb  xmm1, xmm0
        0x66, 0x0F, 0xF6, 0xCB,             // psadbw xmm1, xmm3
        0x66, 0x48, 0x0F, 0x7E, 0xC8,       // movq   rax, xmm1
        0x66, 0x48, 0x0F, 0x3A, 0x16, 0xCA, 0x01,  // pextrq rdx, xmm1, 1
    };
    constexpr int kSteps = 10;

    // Field names long enough to reach the vector loop, plus patterns that
    // separate the two 64-bit halves PSADBW folds independently.
    const std::vector<std::string> inputs = {
        "spotify_track_id",
        "________abcdefgh",
        "abcdefgh________",
        std::string(16, '_'),
        std::string(16, 'a'),
        std::string("\x80\xFF_\x7F\x00_zz\xC0_aa\xFF__z", 16),
    };

    for (const std::string& input : inputs) {
        ASSERT_EQ(input.size(), 16u);

        auto write = m_memory->write_bytes(
            kData, reinterpret_cast<const u8*>(input.data()), input.size());
        ASSERT_TRUE(write);

        m_cpu->rip = 0x1000;
        m_cpu->gpr.set64(static_cast<u8>(RegIndex::RCX), kData);
        m_cpu->gpr.set64(static_cast<u8>(RegIndex::RDI), kNeedle);
        for (int i = 0; i < 4; ++i) {
            std::memset(&m_cpu->sse.xmm[i], 0, sizeof(m_cpu->sse.xmm[i]));
        }

        write_code(code, sizeof(code));
        for (int step = 0; step < kSteps; ++step) {
            ASSERT_EQ(execute_one(), ExecResult::Continue)
                << "step " << step << " of \"" << input << "\"";
        }

        const u64 counted = m_cpu->gpr.get64(static_cast<u8>(RegIndex::RAX)) +
                            m_cpu->gpr.get64(static_cast<u8>(RegIndex::RDX));
        const u64 expected = static_cast<u64>(
            std::count(input.begin(), input.end(), static_cast<char>(kNeedle)));
        EXPECT_EQ(counted, expected)
            << "CRT vectorized count disagrees with the scalar count for \""
            << input << "\"";
    }
}

// ============================================================================
// 15. PAND / POR / PXOR logical
// ============================================================================
TEST_F(SseIntTest, PandPorPxor) {
    // --- PAND ---
    m_cpu->sse.xmm[0].u64_[0] = 0xFF00FF00FF00FF00ULL;
    m_cpu->sse.xmm[0].u64_[1] = 0xAAAAAAAAAAAAAAAAULL;
    m_cpu->sse.xmm[1].u64_[0] = 0x0F0F0F0F0F0F0F0FULL;
    m_cpu->sse.xmm[1].u64_[1] = 0x5555555555555555ULL;

    // 66 0F DB C1 — PAND xmm0, xmm1
    u8 code_and[] = { 0x66, 0x0F, 0xDB, 0xC1 };
    write_code(code_and, sizeof(code_and));
    m_cpu->rip = 0x1000;

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[0], 0x0F000F000F000F00ULL);
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[1], 0x0000000000000000ULL);

    // --- POR ---
    m_cpu->sse.xmm[0].u64_[0] = 0xFF00000000000000ULL;
    m_cpu->sse.xmm[0].u64_[1] = 0x0000000000000000ULL;
    m_cpu->sse.xmm[1].u64_[0] = 0x00000000000000FFULL;
    m_cpu->sse.xmm[1].u64_[1] = 0x00000000FFFFFFFFULL;
    m_cpu->rip = 0x1000;

    // 66 0F EB C1 — POR xmm0, xmm1
    u8 code_or[] = { 0x66, 0x0F, 0xEB, 0xC1 };
    write_code(code_or, sizeof(code_or));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[0], 0xFF000000000000FFULL);
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[1], 0x00000000FFFFFFFFULL);

    // --- PXOR ---
    m_cpu->sse.xmm[0].u64_[0] = 0xFFFFFFFFFFFFFFFFULL;
    m_cpu->sse.xmm[0].u64_[1] = 0x1234567890ABCDEFULL;
    m_cpu->sse.xmm[1].u64_[0] = 0xFFFFFFFFFFFFFFFFULL;
    m_cpu->sse.xmm[1].u64_[1] = 0x1234567890ABCDEFULL;
    m_cpu->rip = 0x1000;

    // 66 0F EF C1 — PXOR xmm0, xmm1
    u8 code_xor[] = { 0x66, 0x0F, 0xEF, 0xC1 };
    write_code(code_xor, sizeof(code_xor));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[0], 0ULL);
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[1], 0ULL);
}

// ============================================================================
// 16. PANDN: NOT(dest) AND src
// ============================================================================
TEST_F(SseIntTest, PandnNotDstAndSrc) {
    m_cpu->sse.xmm[0].u64_[0] = 0xFF00FF00FF00FF00ULL;
    m_cpu->sse.xmm[0].u64_[1] = 0xF0F0F0F0F0F0F0F0ULL;
    m_cpu->sse.xmm[1].u64_[0] = 0xFFFFFFFFFFFFFFFFULL;
    m_cpu->sse.xmm[1].u64_[1] = 0xFFFFFFFFFFFFFFFFULL;

    // 66 0F DF C1 — PANDN xmm0, xmm1
    u8 code[] = { 0x66, 0x0F, 0xDF, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // ~0xFF00FF00FF00FF00 & 0xFFFFFFFFFFFFFFFF = 0x00FF00FF00FF00FF
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[0], 0x00FF00FF00FF00FFULL);
    // ~0xF0F0F0F0F0F0F0F0 & 0xFFFFFFFFFFFFFFFF = 0x0F0F0F0F0F0F0F0F
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[1], 0x0F0F0F0F0F0F0F0FULL);
}

// ============================================================================
// Decoder Tests
// ============================================================================

TEST(SseIntDecoder, PaddbXmm0Xmm1) {
    // 66 0F FC C1 — PADDB xmm0, xmm1
    auto inst = decode({0x66, 0x0F, 0xFC, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::PADDB_XMM_XMMM128);
    EXPECT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg128);
    EXPECT_EQ(inst.operands[0].reg, 0);
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg128);
    EXPECT_EQ(inst.operands[1].reg, 1);
}

TEST(SseIntDecoder, PxorXmm2Xmm3) {
    // 66 0F EF D3 — PXOR xmm2, xmm3
    auto inst = decode({0x66, 0x0F, 0xEF, 0xD3});
    EXPECT_EQ(inst.opcode, Opcode::PXOR_XMM_XMMM128);
    EXPECT_EQ(inst.operands[0].reg, 2);
    EXPECT_EQ(inst.operands[1].reg, 3);
}

TEST(SseIntDecoder, PmaddwdXmm0Xmm1) {
    // 66 0F F5 C1 — PMADDWD xmm0, xmm1
    auto inst = decode({0x66, 0x0F, 0xF5, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::PMADDWD_XMM_XMMM128);
}

TEST(SseIntDecoder, PsadbwXmm4Xmm5) {
    // 66 0F F6 E5 — PSADBW xmm4, xmm5
    auto inst = decode({0x66, 0x0F, 0xF6, 0xE5});
    EXPECT_EQ(inst.opcode, Opcode::PSADBW_XMM_XMMM128);
    EXPECT_EQ(inst.operands[0].reg, 4);
    EXPECT_EQ(inst.operands[1].reg, 5);
}

// ============================================================================
// 32-bit Mode SSE Tests (Ed25519 runs in 32-bit protected mode)
// ============================================================================

class SseInt32Test : public ::testing::Test {
protected:
    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        // Map at 0x10000 for code, at 0x20000 for data
        auto map_code = m_memory->map_region(0x10000, 0x2000, MemoryProtection::ReadWrite);
        auto map_data = m_memory->map_region(0x20000, 0x2000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(map_code);
        ASSERT_TRUE(map_data);

        m_cpu = std::make_unique<CpuState>();
        m_cpu->init_protected_mode();  // 32-bit mode!
        m_cpu->gpr.set32(static_cast<u8>(RegIndex::RSP), 0x21000);
        m_cpu->gpr.set32(static_cast<u8>(RegIndex::EBP), 0x21000);
        m_cpu->rip = 0x10000;
    }

    void write_code(const u8* code, usize len) {
        auto result = m_memory->write_bytes(0x10000, code, len);
        ASSERT_TRUE(result);
    }

    void write_data(u32 addr, const u8* data, usize len) {
        auto result = m_memory->write_bytes(addr, data, len);
        ASSERT_TRUE(result);
    }

    ExecResult execute_one() {
        Interpreter interp(*m_cpu, *m_memory);
        return interp.step();
    }

    std::unique_ptr<GuestMemory> m_memory;
    std::unique_ptr<CpuState> m_cpu;
};

// Test PMULUDQ with memory operand in 32-bit mode
TEST_F(SseInt32Test, Pmuludq32_MemoryOperand) {
    // PMULUDQ xmm0, [ebp-0x10]
    // 66 0F F4 45 F0
    auto& dst = m_cpu->sse.xmm[0];
    std::memset(&dst, 0, sizeof(dst));
    dst.u32_[0] = 0x12345678;
    dst.u32_[2] = 0x11111111;

    // Write source data at ebp-0x10 = 0x21000 - 0x10 = 0x20FF0
    u8 src_data[16] = {0};
    *(u32*)&src_data[0] = 0x2;       // low dword
    *(u32*)&src_data[8] = 0x3;       // high dword (at offset 8)
    write_data(0x20FF0, src_data, 16);

    // 66 0F F4 45 F0 = PMULUDQ xmm0, [ebp-0x10]
    u8 code[] = { 0x66, 0x0F, 0xF4, 0x45, 0xF0 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // u64[0] = 0x12345678 * 2 = 0x2468ACF0
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[0], 0x2468ACF0ULL);
    // u64[1] = 0x11111111 * 3 = 0x33333333
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[1], 0x33333333ULL);
}

// Test PADDQ with memory operand in 32-bit mode
TEST_F(SseInt32Test, Paddq32_MemoryOperand) {
    // PADDQ xmm1, [ebp-0x20]
    // 66 0F D4 4D E0
    auto& dst = m_cpu->sse.xmm[1];
    std::memset(&dst, 0, sizeof(dst));
    dst.u64_[0] = 0x100000000ULL;
    dst.u64_[1] = 0x200000000ULL;

    // Write source at ebp-0x20 = 0x21000 - 0x20 = 0x20FE0
    u8 src_data[16] = {0};
    *(u64*)&src_data[0] = 0x1ULL;
    *(u64*)&src_data[8] = 0x2ULL;
    write_data(0x20FE0, src_data, 16);

    // 66 0F D4 4D E0 = PADDQ xmm1, [ebp-0x20]
    u8 code[] = { 0x66, 0x0F, 0xD4, 0x4D, 0xE0 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[1].u64_[0], 0x100000001ULL);
    EXPECT_EQ(m_cpu->sse.xmm[1].u64_[1], 0x200000002ULL);
}

// Test PSRLQ with immediate in 32-bit mode (Ed25519 uses this for carries)
TEST_F(SseInt32Test, Psrlq32_Imm8) {
    // PSRLQ xmm2, 26
    // 66 0F 73 D2 1A
    auto& xmm = m_cpu->sse.xmm[2];
    std::memset(&xmm, 0, sizeof(xmm));
    xmm.u64_[0] = 0xFFFFFFFFFFFFFFFFULL;
    xmm.u64_[1] = 0x0000000100000000ULL;

    // 66 0F 73 D2 1A = PSRLQ xmm2, 26
    u8 code[] = { 0x66, 0x0F, 0x73, 0xD2, 0x1A };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // >> 26 each qword
    EXPECT_EQ(m_cpu->sse.xmm[2].u64_[0], 0x3FFFFFFFFFULL);  // 0xFFFF... >> 26
    EXPECT_EQ(m_cpu->sse.xmm[2].u64_[1], 64ULL);            // 0x100000000 >> 26 = 2^6 = 64
}

// Test PUNPCKLQDQ with memory operand in 32-bit mode
TEST_F(SseInt32Test, Punpcklqdq32_MemoryOperand) {
    // PUNPCKLQDQ xmm0, [0x20100]
    // 66 0F 6C 05 00 01 02 00
    auto& dst = m_cpu->sse.xmm[0];
    std::memset(&dst, 0, sizeof(dst));
    dst.u64_[0] = 0x1111111122222222ULL;
    dst.u64_[1] = 0x3333333344444444ULL;

    // Write source at 0x20100
    u8 src_data[16] = {0};
    *(u64*)&src_data[0] = 0xAAAAAAAABBBBBBBBULL;
    *(u64*)&src_data[8] = 0xCCCCCCCCDDDDDDDDULL;
    write_data(0x20100, src_data, 16);

    // 66 0F 6C 05 00 01 02 00 = PUNPCKLQDQ xmm0, [0x20100]
    u8 code[] = { 0x66, 0x0F, 0x6C, 0x05, 0x00, 0x01, 0x02, 0x00 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // PUNPCKLQDQ: dst = {dst.u64[0], src.u64[0]}
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[0], 0x1111111122222222ULL);  // dst low
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[1], 0xAAAAAAAABBBBBBBBULL);  // src low
}

// Test PUNPCKHQDQ in 32-bit mode (Ed25519 uses this 24x)
TEST_F(SseInt32Test, Punpckhqdq32_MemoryOperand) {
    // PUNPCKHQDQ xmm1, [0x20100]
    auto& dst = m_cpu->sse.xmm[1];
    std::memset(&dst, 0, sizeof(dst));
    dst.u64_[0] = 0x1111111122222222ULL;
    dst.u64_[1] = 0x3333333344444444ULL;

    // Write source at 0x20100
    u8 src_data[16] = {0};
    *(u64*)&src_data[0] = 0xAAAAAAAABBBBBBBBULL;
    *(u64*)&src_data[8] = 0xCCCCCCCCDDDDDDDDULL;
    write_data(0x20100, src_data, 16);

    // 66 0F 6D 0D 00 01 02 00 = PUNPCKHQDQ xmm1, [0x20100]
    u8 code[] = { 0x66, 0x0F, 0x6D, 0x0D, 0x00, 0x01, 0x02, 0x00 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // PUNPCKHQDQ: dst = {dst.u64[1], src.u64[1]}
    EXPECT_EQ(m_cpu->sse.xmm[1].u64_[0], 0x3333333344444444ULL);  // dst high
    EXPECT_EQ(m_cpu->sse.xmm[1].u64_[1], 0xCCCCCCCCDDDDDDDDULL);  // src high
}

// Test PUNPCKHDQ in 32-bit mode (Ed25519 uses this 19x)
TEST_F(SseInt32Test, Punpckhdq32_MemoryOperand) {
    // PUNPCKHDQ xmm2, [0x20100]
    auto& dst = m_cpu->sse.xmm[2];
    std::memset(&dst, 0, sizeof(dst));
    dst.u32_[0] = 0x11111111;
    dst.u32_[1] = 0x22222222;
    dst.u32_[2] = 0x33333333;
    dst.u32_[3] = 0x44444444;

    // Write source at 0x20100
    u8 src_data[16] = {0};
    ((u32*)src_data)[0] = 0xAAAAAAAA;
    ((u32*)src_data)[1] = 0xBBBBBBBB;
    ((u32*)src_data)[2] = 0xCCCCCCCC;
    ((u32*)src_data)[3] = 0xDDDDDDDD;
    write_data(0x20100, src_data, 16);

    // 66 0F 6A 15 00 01 02 00 = PUNPCKHDQ xmm2, [0x20100]
    u8 code[] = { 0x66, 0x0F, 0x6A, 0x15, 0x00, 0x01, 0x02, 0x00 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // PUNPCKHDQ: interleave high dwords: dst = {dst[2], src[2], dst[3], src[3]}
    EXPECT_EQ(m_cpu->sse.xmm[2].u32_[0], 0x33333333U);  // dst[2]
    EXPECT_EQ(m_cpu->sse.xmm[2].u32_[1], 0xCCCCCCCCU);  // src[2]
    EXPECT_EQ(m_cpu->sse.xmm[2].u32_[2], 0x44444444U);  // dst[3]
    EXPECT_EQ(m_cpu->sse.xmm[2].u32_[3], 0xDDDDDDDDU);  // src[3]
}

// Test PSHUFD in 32-bit mode (Ed25519 uses this 12x)
TEST_F(SseInt32Test, Pshufd32_MemoryOperand) {
    // PSHUFD xmm3, [0x20100], 0x1B  (reverse order: 0,1,2,3 -> 3,2,1,0)
    // Write source at 0x20100
    u8 src_data[16] = {0};
    ((u32*)src_data)[0] = 0x11111111;
    ((u32*)src_data)[1] = 0x22222222;
    ((u32*)src_data)[2] = 0x33333333;
    ((u32*)src_data)[3] = 0x44444444;
    write_data(0x20100, src_data, 16);

    // 66 0F 70 1D 00 01 02 00 1B = PSHUFD xmm3, [0x20100], 0x1B
    u8 code[] = { 0x66, 0x0F, 0x70, 0x1D, 0x00, 0x01, 0x02, 0x00, 0x1B };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // 0x1B = 0b00_01_10_11 = reverse: dst[0]=src[3], dst[1]=src[2], dst[2]=src[1], dst[3]=src[0]
    EXPECT_EQ(m_cpu->sse.xmm[3].u32_[0], 0x44444444U);
    EXPECT_EQ(m_cpu->sse.xmm[3].u32_[1], 0x33333333U);
    EXPECT_EQ(m_cpu->sse.xmm[3].u32_[2], 0x22222222U);
    EXPECT_EQ(m_cpu->sse.xmm[3].u32_[3], 0x11111111U);
}

// =============================================================================
// SSSE3 Tests (32-bit mode) - Used by SHA-512 in Steam
// =============================================================================

// Test PSHUFB - Packed Shuffle Bytes (used 16x in SHA-512)
TEST_F(SseInt32Test, Pshufb32_Basic) {
    // PSHUFB xmm0, [0x20100]
    // Set up xmm0 with data to shuffle
    auto& dst = m_cpu->sse.xmm[0];
    for (int i = 0; i < 16; ++i) {
        dst.u8_[i] = static_cast<u8>(i);  // 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    }

    // Control mask: reverse bytes
    u8 mask[16] = {15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
    write_data(0x20100, mask, 16);

    // 66 0F 38 00 05 00 01 02 00 = PSHUFB xmm0, [0x20100]
    u8 code[] = { 0x66, 0x0F, 0x38, 0x00, 0x05, 0x00, 0x01, 0x02, 0x00 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // Result should be reversed: 15,14,13,...,0
    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(m_cpu->sse.xmm[0].u8_[i], static_cast<u8>(15 - i));
    }
}

// Test PSHUFB with high bit set (zeros output bytes)
TEST_F(SseInt32Test, Pshufb32_ZeroBits) {
    auto& dst = m_cpu->sse.xmm[1];
    for (int i = 0; i < 16; ++i) {
        dst.u8_[i] = 0xFF;  // All 1s
    }

    // Control mask: bytes 0-7 get normal shuffle, bytes 8-15 get zeroed (high bit set)
    u8 mask[16] = {0, 1, 2, 3, 4, 5, 6, 7, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87};
    write_data(0x20100, mask, 16);

    // 66 0F 38 00 0D 00 01 02 00 = PSHUFB xmm1, [0x20100]
    u8 code[] = { 0x66, 0x0F, 0x38, 0x00, 0x0D, 0x00, 0x01, 0x02, 0x00 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // Bytes 0-7 should be 0xFF (copied from themselves), bytes 8-15 should be 0
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(m_cpu->sse.xmm[1].u8_[i], 0xFFU);
    }
    for (int i = 8; i < 16; ++i) {
        EXPECT_EQ(m_cpu->sse.xmm[1].u8_[i], 0U);
    }
}

// Test PSHUFB for SHA-512 byte swap pattern (big-endian conversion)
TEST_F(SseInt32Test, Pshufb32_ByteSwap) {
    auto& dst = m_cpu->sse.xmm[2];
    // Two 64-bit values in little-endian
    dst.u64_[0] = 0x0102030405060708ULL;
    dst.u64_[1] = 0x1112131415161718ULL;

    // SHA-512 uses PSHUFB to byte-swap for big-endian
    // Reverse bytes within each 64-bit half
    u8 mask[16] = {7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8};
    write_data(0x20100, mask, 16);

    // 66 0F 38 00 15 00 01 02 00 = PSHUFB xmm2, [0x20100]
    u8 code[] = { 0x66, 0x0F, 0x38, 0x00, 0x15, 0x00, 0x01, 0x02, 0x00 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // After byte swap: 0807060504030201, 1817161514131211
    EXPECT_EQ(m_cpu->sse.xmm[2].u64_[0], 0x0807060504030201ULL);
    EXPECT_EQ(m_cpu->sse.xmm[2].u64_[1], 0x1817161514131211ULL);
}

// Test PALIGNR - Packed Align Right (used 16x in SHA-512)
TEST_F(SseInt32Test, Palignr32_Shift8) {
    // PALIGNR xmm0, [mem], 8 - shift concatenated dst:src right by 8 bytes
    auto& dst = m_cpu->sse.xmm[0];
    dst.u64_[0] = 0xDDDDDDDDDDDDDDDDULL;
    dst.u64_[1] = 0xEEEEEEEEEEEEEEEEULL;

    u8 src[16];
    *(u64*)&src[0] = 0xAAAAAAAAAAAAAAAAULL;
    *(u64*)&src[8] = 0xBBBBBBBBBBBBBBBBULL;
    write_data(0x20100, src, 16);

    // 66 0F 3A 0F 05 00 01 02 00 08 = PALIGNR xmm0, [0x20100], 8
    u8 code[] = { 0x66, 0x0F, 0x3A, 0x0F, 0x05, 0x00, 0x01, 0x02, 0x00, 0x08 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // Concatenate dst:src = EEEE..DDDD..:BBBB..AAAA.. (256 bits)
    // Shift right 8 bytes: low 128 bits becomes DDDD..:BBBB..
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[0], 0xBBBBBBBBBBBBBBBBULL);
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[1], 0xDDDDDDDDDDDDDDDDULL);
}

// Test PALIGNR with shift 16 (exactly takes dst portion)
TEST_F(SseInt32Test, Palignr32_Shift16) {
    auto& dst = m_cpu->sse.xmm[1];
    dst.u64_[0] = 0x1111111111111111ULL;
    dst.u64_[1] = 0x2222222222222222ULL;

    u8 src[16];
    *(u64*)&src[0] = 0x3333333333333333ULL;
    *(u64*)&src[8] = 0x4444444444444444ULL;
    write_data(0x20100, src, 16);

    // 66 0F 3A 0F 0D 00 01 02 00 10 = PALIGNR xmm1, [0x20100], 16
    u8 code[] = { 0x66, 0x0F, 0x3A, 0x0F, 0x0D, 0x00, 0x01, 0x02, 0x00, 0x10 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // Shift 16 bytes: result is just dst
    EXPECT_EQ(m_cpu->sse.xmm[1].u64_[0], 0x1111111111111111ULL);
    EXPECT_EQ(m_cpu->sse.xmm[1].u64_[1], 0x2222222222222222ULL);
}

// ============================================================================
// AESKEYGENASSIST — AES-128 key expansion correctness (Intel spec §Table 4-11)
// ============================================================================
//
// AESKEYGENASSIST dst, src, RCON:
//   dst[31:0]   = SubWord(src[63:32])
//   dst[63:32]  = RotWord(SubWord(src[63:32]))  XOR RCON
//   dst[95:64]  = SubWord(src[127:96])
//   dst[127:96] = RotWord(SubWord(src[127:96])) XOR RCON
//
// Test vector: AES-128 initial key = 00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
// src (xmm1) = bytes[0..15] = 00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
// Stored as little-endian u32 words:
//   u32_[0]=0x03020100  u32_[1]=0x07060504  u32_[2]=0x0b0a0908  u32_[3]=0x0f0e0d0c
// RCON = 0x01
// Expected results:
//   SubWord(u32_[1]=0x07060504) = S[04]|S[05]<<8|S[06]<<16|S[07]<<24
//                                = 0xF2|0x6B<<8|0x6F<<16|0xC5<<24 = 0xC56F6BF2
//   RotWord(0xC56F6BF2) = 0xF2C56F6B; XOR 0x01 = 0xF2C56F6A
//   SubWord(u32_[3]=0x0f0e0d0c) = S[0c]|S[0d]<<8|S[0e]<<16|S[0f]<<24
//                                = 0xFE|0xD7<<8|0xAB<<16|0x76<<24 = 0x76ABD7FE
//   RotWord(0x76ABD7FE) = 0xFE76ABD7; XOR 0x01 = 0xFE76ABD6
TEST_F(SseInt32Test, AeskeygenassistVector) {
    // Load src in xmm1 using the AES-128 standard key
    auto& src = m_cpu->sse.xmm[1];
    src.u32_[0] = 0x03020100u;  // bytes  0..3
    src.u32_[1] = 0x07060504u;  // bytes  4..7
    src.u32_[2] = 0x0b0a0908u;  // bytes  8..11
    src.u32_[3] = 0x0f0e0d0cu;  // bytes 12..15

    // dst is xmm0 (zeroed)
    m_cpu->sse.xmm[0].u64_[0] = 0;
    m_cpu->sse.xmm[0].u64_[1] = 0;

    // 66 0F 3A DF C1 01 = AESKEYGENASSIST xmm0, xmm1, 0x01
    u8 code[] = { 0x66, 0x0F, 0x3A, 0xDF, 0xC1, 0x01 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);

    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[0], 0xC56F6BF2u) << "dst[31:0]   = SubWord(src[63:32])";
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[1], 0xF2C56F6Au) << "dst[63:32]  = RotWord(SubWord(src[63:32])) XOR RCON";
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[2], 0x76ABD7FEu) << "dst[95:64]  = SubWord(src[127:96])";
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[3], 0xFE76ABD6u) << "dst[127:96] = RotWord(SubWord(src[127:96])) XOR RCON";
}

// ============================================================================
// PEXTRD r32, xmm, imm8 — decode
// ============================================================================

TEST_F(SseIntTest, PextrdDecode) {
    // 66 0F 3A 16 FB 03 = PEXTRD ebx, xmm7, 3
    auto inst = decode({0x66, 0x0F, 0x3A, 0x16, 0xFB, 0x03}, DecodeMode::Mode32);
    EXPECT_EQ(inst.opcode, Opcode::PEXTRD_RM32_XMM_IMM8);
    EXPECT_EQ(inst.operands[1].reg, 7u);  // xmm7
    EXPECT_EQ(inst.operands[2].imm, 3u);
}

// ============================================================================
// PEXTRD r32, xmm, imm8 — execute
// ============================================================================

TEST_F(SseIntTest, PextrdExec) {
    m_cpu->sse.xmm[2].u32_[0] = 0xAAAAAAAAu;
    m_cpu->sse.xmm[2].u32_[1] = 0xBBBBBBBBu;
    m_cpu->sse.xmm[2].u32_[2] = 0xCCCCCCCCu;
    m_cpu->sse.xmm[2].u32_[3] = 0xDDDDDDDDu;

    // 66 0F 3A 16 D0 02 = PEXTRD eax, xmm2, 2
    u8 code[] = { 0x66, 0x0F, 0x3A, 0x16, 0xD0, 0x02 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(static_cast<u32>(m_cpu->gpr.rax), 0xCCCCCCCCu);
}

// ============================================================================
// PINSRD xmm, r32, imm8 — decode
// ============================================================================

TEST_F(SseIntTest, PinsrdDecode) {
    // 66 0F 3A 22 FD 03 = PINSRD xmm7, ebp, 3
    auto inst = decode({0x66, 0x0F, 0x3A, 0x22, 0xFD, 0x03}, DecodeMode::Mode32);
    EXPECT_EQ(inst.opcode, Opcode::PINSRD_XMM_RM32_IMM8);
    EXPECT_EQ(inst.operands[0].reg, 7u);  // xmm7
    EXPECT_EQ(inst.operands[2].imm, 3u);
}

// ============================================================================
// PINSRD xmm, r32, imm8 — execute
// ============================================================================

TEST_F(SseIntTest, PinsrdExec) {
    m_cpu->sse.xmm[3].u32_[0] = 0x11111111u;
    m_cpu->sse.xmm[3].u32_[1] = 0x22222222u;
    m_cpu->sse.xmm[3].u32_[2] = 0x33333333u;
    m_cpu->sse.xmm[3].u32_[3] = 0x44444444u;
    m_cpu->gpr.rax = 0xDEADBEEFu;

    // 66 0F 3A 22 D8 01 = PINSRD xmm3, eax, 1
    u8 code[] = { 0x66, 0x0F, 0x3A, 0x22, 0xD8, 0x01 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[3].u32_[0], 0x11111111u);
    EXPECT_EQ(m_cpu->sse.xmm[3].u32_[1], 0xDEADBEEFu);
    EXPECT_EQ(m_cpu->sse.xmm[3].u32_[2], 0x33333333u);
    EXPECT_EQ(m_cpu->sse.xmm[3].u32_[3], 0x44444444u);
}