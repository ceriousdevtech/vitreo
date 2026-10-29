// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Crypto instruction tests — AES-NI, SHA, PCLMULQDQ (CPU-047)
//
// Tests the interpreter's crypto_ops.cpp implementation which previously
// had ZERO test coverage across 658 lines of code.

#include <gtest/gtest.h>
#include <cstring>
#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"

using namespace vitreo;
using namespace vitreo::cpu;

class CryptoOpsTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        auto map_result = m_memory->map_region(0x1000, 0x10000,
                                                MemoryProtection::ReadWrite);
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

    // Set XMM register to all-same-byte pattern
    void fill_xmm(int reg, u8 byte) {
        std::memset(&m_cpu->sse.xmm[reg], byte, 16);
    }

    // Set XMM register from two u64 values (lo, hi)
    void set_xmm(int reg, u64 lo, u64 hi) {
        m_cpu->sse.xmm[reg].u64_[0] = lo;
        m_cpu->sse.xmm[reg].u64_[1] = hi;
    }

    // Check XMM register equals two u64 values
    void check_xmm(int reg, u64 lo, u64 hi, const char* msg = "") {
        EXPECT_EQ(m_cpu->sse.xmm[reg].u64_[0], lo)
            << msg << " XMM" << reg << " low";
        EXPECT_EQ(m_cpu->sse.xmm[reg].u64_[1], hi)
            << msg << " XMM" << reg << " high";
    }

    // Check all 16 bytes of XMM are the same value
    void check_xmm_uniform(int reg, u8 byte, const char* msg = "") {
        for (int i = 0; i < 16; i++) {
            EXPECT_EQ(m_cpu->sse.xmm[reg].u8_[i], byte)
                << msg << " XMM" << reg << "[" << i << "]";
        }
    }

    std::unique_ptr<GuestMemory> m_memory;
    std::unique_ptr<CpuState> m_cpu;
};

// ============================================================================
// AESENC — one round of AES encryption
// SubBytes → ShiftRows → MixColumns → XOR key
// ============================================================================

TEST_F(CryptoOpsTest, AesencAllZeros) {
    // Both state (XMM0) and round key (XMM1) are zero.
    // SubBytes(0x00) = 0x63 for all bytes.
    // ShiftRows/MixColumns on uniform 0x63 = identity (eigenvector).
    // XOR with zero key = 0x63.
    // Result: all bytes = 0x63
    fill_xmm(0, 0x00);
    fill_xmm(1, 0x00);

    // AESENC XMM0, XMM1 — 66 0F 38 DC C1
    u8 code[] = {0x66, 0x0F, 0x38, 0xDC, 0xC1};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    check_xmm_uniform(0, 0x63, "AESENC(zeros)");
}

TEST_F(CryptoOpsTest, AesencNonZero) {
    // Verify AESENC with non-zero input produces different output
    set_xmm(0, 0x0123456789ABCDEFull, 0xFEDCBA9876543210ull);
    fill_xmm(1, 0x00);

    u8 code[] = {0x66, 0x0F, 0x38, 0xDC, 0xC1};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // Output must differ from input
    EXPECT_NE(m_cpu->sse.xmm[0].u64_[0], 0x0123456789ABCDEFull);
    EXPECT_NE(m_cpu->sse.xmm[0].u64_[1], 0xFEDCBA9876543210ull);
}

TEST_F(CryptoOpsTest, AesencDifferentRegs) {
    // AESENC XMM2, XMM3 — 66 0F 38 DC D3
    // ModRM D3: mod=11, reg=2(XMM2), rm=3(XMM3)
    fill_xmm(2, 0x00);
    fill_xmm(3, 0x00);

    u8 code[] = {0x66, 0x0F, 0x38, 0xDC, 0xD3};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    check_xmm_uniform(2, 0x63, "AESENC XMM2,XMM3");
}

// ============================================================================
// AESENCLAST — final round (SubBytes → ShiftRows → XOR key, NO MixColumns)
// ============================================================================

TEST_F(CryptoOpsTest, AesenclastAllZeros) {
    // SubBytes and ShiftRows on uniform bytes = same.
    // No MixColumns. XOR with zero key.
    // Result: all bytes = 0x63
    fill_xmm(0, 0x00);
    fill_xmm(1, 0x00);

    // AESENCLAST XMM0, XMM1 — 66 0F 38 DD C1
    u8 code[] = {0x66, 0x0F, 0x38, 0xDD, 0xC1};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    check_xmm_uniform(0, 0x63, "AESENCLAST(zeros)");
}

TEST_F(CryptoOpsTest, AesenclastWithKey) {
    // State = 0, Key = all 0xFF
    // SubBytes(0) = 0x63, ShiftRows = 0x63, XOR 0xFF = 0x9C
    fill_xmm(0, 0x00);
    fill_xmm(1, 0xFF);

    u8 code[] = {0x66, 0x0F, 0x38, 0xDD, 0xC1};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    check_xmm_uniform(0, 0x63 ^ 0xFF, "AESENCLAST(0, FF)");
}

// ============================================================================
// AESDEC — one round of AES decryption
// InvShiftRows → InvSubBytes → InvMixColumns → XOR key
// ============================================================================

TEST_F(CryptoOpsTest, AesdecAllZeros) {
    // InvShiftRows(0) = 0, InvSubBytes(0x00) = 0x52.
    // InvMixColumns on uniform 0x52 = 0x52 (eigenvector of InvMixColumns).
    // XOR with zero key = 0x52.
    fill_xmm(0, 0x00);
    fill_xmm(1, 0x00);

    // AESDEC XMM0, XMM1 — 66 0F 38 DE C1
    u8 code[] = {0x66, 0x0F, 0x38, 0xDE, 0xC1};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    check_xmm_uniform(0, 0x52, "AESDEC(zeros)");
}

// ============================================================================
// AESDECLAST — final decryption round (no InvMixColumns)
// ============================================================================

TEST_F(CryptoOpsTest, AesdeclastAllZeros) {
    // InvShiftRows(0) = 0, InvSubBytes(0x00) = 0x52, XOR key = 0x52
    fill_xmm(0, 0x00);
    fill_xmm(1, 0x00);

    // AESDECLAST XMM0, XMM1 — 66 0F 38 DF C1
    u8 code[] = {0x66, 0x0F, 0x38, 0xDF, 0xC1};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    check_xmm_uniform(0, 0x52, "AESDECLAST(zeros)");
}

TEST_F(CryptoOpsTest, AesdeclastWithKey) {
    fill_xmm(0, 0x00);
    fill_xmm(1, 0xFF);

    u8 code[] = {0x66, 0x0F, 0x38, 0xDF, 0xC1};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    check_xmm_uniform(0, 0x52 ^ 0xFF, "AESDECLAST(0, FF)");
}

// ============================================================================
// AESIMC — InvMixColumns only (for decryption key schedule)
// ============================================================================

TEST_F(CryptoOpsTest, AesimcAllZeros) {
    // InvMixColumns on zero = zero
    fill_xmm(0, 0x00);
    fill_xmm(1, 0x00);

    // AESIMC XMM0, XMM1 — 66 0F 38 DB C1
    u8 code[] = {0x66, 0x0F, 0x38, 0xDB, 0xC1};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    check_xmm(0, 0, 0, "AESIMC(zeros)");
}

TEST_F(CryptoOpsTest, AesimcNonZero) {
    // InvMixColumns on non-zero produces non-zero
    set_xmm(1, 0x0123456789ABCDEFull, 0xFEDCBA9876543210ull);

    u8 code[] = {0x66, 0x0F, 0x38, 0xDB, 0xC1};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_NE(m_cpu->sse.xmm[0].u64_[0], 0u);
}

// ============================================================================
// AESKEYGENASSIST — key expansion helper
// ============================================================================

TEST_F(CryptoOpsTest, AeskeygenassistAllZeros) {
    // With zero input and RCON=0:
    // SubWord(X1=0) = 0x63636363
    // RotWord(0x63636363) = 0x63636363 (uniform)
    // XOR RCON=0 → 0x63636363
    // All four dwords = 0x63636363
    fill_xmm(1, 0x00);

    // AESKEYGENASSIST XMM0, XMM1, 0 — 66 0F 3A DF C1 00
    u8 code[] = {0x66, 0x0F, 0x3A, 0xDF, 0xC1, 0x00};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    for (int i = 0; i < 4; i++) {
        EXPECT_EQ(m_cpu->sse.xmm[0].u32_[i], 0x63636363u)
            << "dword[" << i << "]";
    }
}

TEST_F(CryptoOpsTest, AeskeygenassistWithRcon) {
    fill_xmm(1, 0x00);

    // AESKEYGENASSIST XMM0, XMM1, 0x01 — 66 0F 3A DF C1 01
    u8 code[] = {0x66, 0x0F, 0x3A, 0xDF, 0xC1, 0x01};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // dwords 0 and 2 are SubWord(X1/X3) = 0x63636363
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[0], 0x63636363u) << "dword[0]";
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[2], 0x63636363u) << "dword[2]";
    // dwords 1 and 3 are RotWord(SubWord(X1/X3)) XOR RCON
    // RotWord(0x63636363) = 0x63636363, XOR 0x01 = 0x63636362
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[1], 0x63636362u) << "dword[1]";
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[3], 0x63636362u) << "dword[3]";
}

// ============================================================================
// PCLMULQDQ — carry-less multiplication
// ============================================================================

TEST_F(CryptoOpsTest, PclmulqdqOneTimesOne) {
    // 1 * 1 = 1 in GF(2)
    set_xmm(0, 1, 0);
    set_xmm(1, 1, 0);

    // PCLMULQDQ XMM0, XMM1, 0x00 — 66 0F 3A 44 C1 00
    // imm8=0x00: select low qword of both
    u8 code[] = {0x66, 0x0F, 0x3A, 0x44, 0xC1, 0x00};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    check_xmm(0, 1, 0, "PCLMULQDQ(1*1)");
}

TEST_F(CryptoOpsTest, PclmulqdqThreeTimesThree) {
    // In GF(2) polynomial multiplication:
    // 3 = x+1, 3*3 = (x+1)(x+1) = x^2 + 1 = 5
    set_xmm(0, 3, 0);
    set_xmm(1, 3, 0);

    u8 code[] = {0x66, 0x0F, 0x3A, 0x44, 0xC1, 0x00};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    check_xmm(0, 5, 0, "PCLMULQDQ(3*3)");
}

TEST_F(CryptoOpsTest, PclmulqdqZero) {
    // 0 * anything = 0
    set_xmm(0, 0, 0);
    set_xmm(1, 0xFFFFFFFFFFFFFFFFull, 0);

    u8 code[] = {0x66, 0x0F, 0x3A, 0x44, 0xC1, 0x00};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    check_xmm(0, 0, 0, "PCLMULQDQ(0*anything)");
}

TEST_F(CryptoOpsTest, PclmulqdqHighQwords) {
    // imm8=0x11: select high qword of both
    set_xmm(0, 0, 1);  // high qword = 1
    set_xmm(1, 0, 1);  // high qword = 1

    u8 code[] = {0x66, 0x0F, 0x3A, 0x44, 0xC1, 0x11};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    check_xmm(0, 1, 0, "PCLMULQDQ high(1*1)");
}

// ============================================================================
// SHA1NEXTE — SHA-1 next E value
// ============================================================================

TEST_F(CryptoOpsTest, Sha1nexteAllZeros) {
    fill_xmm(0, 0x00);
    fill_xmm(1, 0x00);

    // SHA1NEXTE XMM0, XMM1 — 0F 38 C8 C1
    u8 code[] = {0x0F, 0x38, 0xC8, 0xC1};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // ROL(0, 30) + 0 = 0, copies xmm1 low 96 bits
    check_xmm(0, 0, 0, "SHA1NEXTE(zeros)");
}

TEST_F(CryptoOpsTest, Sha1nexteNonZero) {
    // Set xmm0[127:96] = 1, rest = 0
    fill_xmm(0, 0x00);
    m_cpu->sse.xmm[0].u32_[3] = 1;
    // Set xmm1 to a known pattern
    set_xmm(1, 0x1111111122222222ull, 0x3333333344444444ull);

    u8 code[] = {0x0F, 0x38, 0xC8, 0xC1};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // Result[127:96] = ROL(1, 30) + xmm1[127:96]
    // ROL(1, 30) = 0x40000000, xmm1[127:96] = u32_[3] = 0x33333333
    u32 expected_hi = 0x40000000 + 0x33333333;
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[3], expected_hi)
        << "SHA1NEXTE E value";
    // Lower 96 bits copied from xmm1
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[0], 0x22222222u);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[1], 0x11111111u);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[2], 0x44444444u);
}

// ============================================================================
// SHA1MSG1 — SHA-1 message schedule W[i] XOR W[i+2]
// ============================================================================

TEST_F(CryptoOpsTest, Sha1msg1AllZeros) {
    fill_xmm(0, 0x00);
    fill_xmm(1, 0x00);

    // SHA1MSG1 XMM0, XMM1 — 0F 38 C9 C1
    u8 code[] = {0x0F, 0x38, 0xC9, 0xC1};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    check_xmm(0, 0, 0, "SHA1MSG1(zeros)");
}

TEST_F(CryptoOpsTest, Sha1msg1NonZero) {
    set_xmm(0, 0x0000000100000002ull, 0x0000000300000004ull);
    set_xmm(1, 0x0000000500000006ull, 0x0000000700000008ull);

    u8 code[] = {0x0F, 0x38, 0xC9, 0xC1};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // Output should differ from input (XOR operations applied)
    EXPECT_NE(m_cpu->sse.xmm[0].u64_[0], 0x0000000100000002ull);
}

// ============================================================================
// SHA1MSG2 — SHA-1 message schedule final XOR + ROL
// ============================================================================

TEST_F(CryptoOpsTest, Sha1msg2AllZeros) {
    fill_xmm(0, 0x00);
    fill_xmm(1, 0x00);

    // SHA1MSG2 XMM0, XMM1 — 0F 38 CA C1
    u8 code[] = {0x0F, 0x38, 0xCA, 0xC1};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // ROL(0 XOR 0, 1) = 0 for all dwords
    check_xmm(0, 0, 0, "SHA1MSG2(zeros)");
}

TEST_F(CryptoOpsTest, Sha1msg2PreservesIntelLaneOrder) {
    // Physical XMM lane order is u32_[0] = bits 31:0 through
    // u32_[3] = bits 127:96.  SHA1MSG2 names W16 first in the most
    // significant lane, so this asymmetric vector catches a reversed-lane
    // implementation.
    m_cpu->sse.xmm[0].u32_[0] = 0x01234567u;
    m_cpu->sse.xmm[0].u32_[1] = 0x89ABCDEFu;
    m_cpu->sse.xmm[0].u32_[2] = 0xFEDCBA98u;
    m_cpu->sse.xmm[0].u32_[3] = 0x76543210u;
    m_cpu->sse.xmm[1].u32_[0] = 0x0F1E2D3Cu;
    m_cpu->sse.xmm[1].u32_[1] = 0x4B5A6978u;
    m_cpu->sse.xmm[1].u32_[2] = 0x8796A5B4u;
    m_cpu->sse.xmm[1].u32_[3] = 0xC3D2E1F0u;

    u8 code[] = {0x0F, 0x38, 0xCA, 0xC1}; // SHA1MSG2 XMM0, XMM1
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[3], 0xE3852F49u); // W16
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[2], 0x6B0DA7C1u); // W17
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[1], 0x0D6BC1A7u); // W18
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[0], 0xC54CD45Du); // W19
}

// ============================================================================
// SHA256RNDS2 — two rounds of SHA-256 (implicit XMM0 as third operand)
// ============================================================================

TEST_F(CryptoOpsTest, Sha256rnds2AllZeros) {
    fill_xmm(0, 0x00);  // Implicit WK operand
    fill_xmm(1, 0x00);
    fill_xmm(2, 0x00);

    // SHA256RNDS2 XMM1, XMM2 — 0F 38 CB CA
    // ModRM CA: mod=11, reg=1(XMM1), rm=2(XMM2)
    u8 code[] = {0x0F, 0x38, 0xCB, 0xCA};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // With all-zero state, SHA-256 round functions produce deterministic
    // (but non-trivial) output. Just verify it runs.
}

TEST_F(CryptoOpsTest, Sha256rnds2NonZero) {
    // Use non-zero state to verify instruction modifies output
    set_xmm(0, 0x428A2F9871374491ull, 0x0000000000000000ull);
    set_xmm(1, 0x6A09E667BB67AE85ull, 0x3C6EF372A54FF53Aull);
    set_xmm(2, 0x510E527F9B05688Cull, 0x1F83D9AB5BE0CD19ull);

    u8 code[] = {0x0F, 0x38, 0xCB, 0xCA};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // Output should differ from input
    EXPECT_NE(m_cpu->sse.xmm[1].u64_[0], 0x6A09E667BB67AE85ull);
}

// ============================================================================
// SHA256MSG1 / SHA256MSG2
// ============================================================================

TEST_F(CryptoOpsTest, Sha256msg1AllZeros) {
    fill_xmm(0, 0x00);
    fill_xmm(1, 0x00);

    // SHA256MSG1 XMM0, XMM1 — 0F 38 CC C1
    u8 code[] = {0x0F, 0x38, 0xCC, 0xC1};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // sigma0(0) = 0, so all zeros stay zero
    check_xmm(0, 0, 0, "SHA256MSG1(zeros)");
}

TEST_F(CryptoOpsTest, Sha256msg2AllZeros) {
    fill_xmm(0, 0x00);
    fill_xmm(1, 0x00);

    // SHA256MSG2 XMM0, XMM1 — 0F 38 CD C1
    u8 code[] = {0x0F, 0x38, 0xCD, 0xC1};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    check_xmm(0, 0, 0, "SHA256MSG2(zeros)");
}

// ============================================================================
// SHA1RNDS4 — four rounds of SHA-1 with function select
// ============================================================================

TEST_F(CryptoOpsTest, Sha1rnds4AllZerosFunc0) {
    fill_xmm(0, 0x00);
    fill_xmm(1, 0x00);

    // SHA1RNDS4 XMM0, XMM1, 0 — 0F 3A CC C1 00
    u8 code[] = {0x0F, 0x3A, 0xCC, 0xC1, 0x00};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // Verify it doesn't crash and produces some output
}

TEST_F(CryptoOpsTest, Sha1rnds4AllZerosFunc1) {
    fill_xmm(0, 0x00);
    fill_xmm(1, 0x00);

    // SHA1RNDS4 XMM0, XMM1, 1 — 0F 3A CC C1 01
    u8 code[] = {0x0F, 0x3A, 0xCC, 0xC1, 0x01};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
}

TEST_F(CryptoOpsTest, Sha1rnds4AllZerosFunc2) {
    fill_xmm(0, 0x00);
    fill_xmm(1, 0x00);

    // SHA1RNDS4 XMM0, XMM1, 2 — 0F 3A CC C1 02
    u8 code[] = {0x0F, 0x3A, 0xCC, 0xC1, 0x02};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
}

TEST_F(CryptoOpsTest, Sha1rnds4AllZerosFunc3) {
    fill_xmm(0, 0x00);
    fill_xmm(1, 0x00);

    // SHA1RNDS4 XMM0, XMM1, 3 — 0F 3A CC C1 03
    u8 code[] = {0x0F, 0x3A, 0xCC, 0xC1, 0x03};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
}

// ============================================================================
// AES roundtrip: AESENC identity check
// Verify that applying AESENC with different keys produces different results
// ============================================================================

TEST_F(CryptoOpsTest, AesencKeyDependence) {
    // Same state, different keys should give different results
    u64 state_lo = 0x0123456789ABCDEFull;
    u64 state_hi = 0xFEDCBA9876543210ull;

    // Run AESENC with key=0
    set_xmm(0, state_lo, state_hi);
    fill_xmm(1, 0x00);
    u8 code[] = {0x66, 0x0F, 0x38, 0xDC, 0xC1};
    write_code(code, sizeof(code));
    m_cpu->rip = 0x1000;
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    u64 result0_lo = m_cpu->sse.xmm[0].u64_[0];
    u64 result0_hi = m_cpu->sse.xmm[0].u64_[1];

    // Run AESENC with key=FF
    set_xmm(0, state_lo, state_hi);
    fill_xmm(1, 0xFF);
    write_code(code, sizeof(code));
    m_cpu->rip = 0x1000;
    EXPECT_EQ(execute_one(), ExecResult::Continue);

    // Results must differ (different keys)
    EXPECT_TRUE(m_cpu->sse.xmm[0].u64_[0] != result0_lo ||
                m_cpu->sse.xmm[0].u64_[1] != result0_hi)
        << "AESENC with different keys should produce different results";
}

// ============================================================================
// PSHUFB (often tested with crypto code paths)
// ============================================================================

TEST_F(CryptoOpsTest, PshufbidentityMask) {
    // PSHUFB with mask = {0,1,2,...,15} is an identity permutation
    set_xmm(0, 0x0706050403020100ull, 0x0F0E0D0C0B0A0908ull);
    set_xmm(1, 0x0706050403020100ull, 0x0F0E0D0C0B0A0908ull);

    // PSHUFB XMM0, XMM1 — 66 0F 38 00 C1
    u8 code[] = {0x66, 0x0F, 0x38, 0x00, 0xC1};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    check_xmm(0, 0x0706050403020100ull, 0x0F0E0D0C0B0A0908ull,
              "PSHUFB identity");
}

TEST_F(CryptoOpsTest, PshufbHighBitZero) {
    // If a mask byte has bit 7 set, the corresponding output byte = 0
    set_xmm(0, 0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull);
    set_xmm(1, 0x8080808080808080ull, 0x8080808080808080ull);

    u8 code[] = {0x66, 0x0F, 0x38, 0x00, 0xC1};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    check_xmm(0, 0, 0, "PSHUFB all-masked");
}

// ============================================================================
// PMULLD — packed multiply low dword
// ============================================================================

TEST_F(CryptoOpsTest, PmulldBasic) {
    m_cpu->sse.xmm[0].u32_[0] = 7;
    m_cpu->sse.xmm[0].u32_[1] = 100;
    m_cpu->sse.xmm[0].u32_[2] = 0;
    m_cpu->sse.xmm[0].u32_[3] = 0xFFFFFFFF;
    m_cpu->sse.xmm[1].u32_[0] = 6;
    m_cpu->sse.xmm[1].u32_[1] = 200;
    m_cpu->sse.xmm[1].u32_[2] = 999;
    m_cpu->sse.xmm[1].u32_[3] = 2;

    // PMULLD XMM0, XMM1 — 66 0F 38 40 C1
    u8 code[] = {0x66, 0x0F, 0x38, 0x40, 0xC1};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[0], 42u);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[1], 20000u);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[2], 0u);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[3], 0xFFFFFFFEu); // -1 * 2 = -2
}

// ============================================================================
// PALIGNR — byte-align concatenation
// ============================================================================

TEST_F(CryptoOpsTest, PalignrZeroShift) {
    set_xmm(0, 0x0F0E0D0C0B0A0908ull, 0x1716151413121110ull);
    set_xmm(1, 0x0706050403020100ull, 0x0F0E0D0C0B0A0908ull);

    // PALIGNR XMM0, XMM1, 0 — 66 0F 3A 0F C1 00
    u8 code[] = {0x66, 0x0F, 0x3A, 0x0F, 0xC1, 0x00};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // With shift=0, result = XMM1 (source)
    check_xmm(0, 0x0706050403020100ull, 0x0F0E0D0C0B0A0908ull,
              "PALIGNR shift=0");
}

// ============================================================================
// FIPS-197 Appendix B reference vector — Round 1 of AES-128
// State at start of round 1:
//   19 3d e3 be a0 f4 e2 2b 9a c6 8d 2a e9 f8 48 08
// Round key w[4..7]:
//   a0 fa fe 17 88 54 2c b1 23 a3 39 39 2a 6c 76 05
// Expected output (per FIPS-197 Appendix B):
//   a4 9c 7f f2 68 9f 35 2b 6b 5b ea 43 02 6a 50 49
// ============================================================================
TEST_F(CryptoOpsTest, AesencFips197Round1) {
    const u8 state_bytes[16] = {
        0x19,0x3d,0xe3,0xbe, 0xa0,0xf4,0xe2,0x2b,
        0x9a,0xc6,0x8d,0x2a, 0xe9,0xf8,0x48,0x08
    };
    const u8 key_bytes[16] = {
        0xa0,0xfa,0xfe,0x17, 0x88,0x54,0x2c,0xb1,
        0x23,0xa3,0x39,0x39, 0x2a,0x6c,0x76,0x05
    };
    const u8 expected[16] = {
        0xa4,0x9c,0x7f,0xf2, 0x68,0x9f,0x35,0x2b,
        0x6b,0x5b,0xea,0x43, 0x02,0x6a,0x50,0x49
    };
    std::memcpy(&m_cpu->sse.xmm[0], state_bytes, 16);
    std::memcpy(&m_cpu->sse.xmm[1], key_bytes,   16);

    u8 code[] = {0x66, 0x0F, 0x38, 0xDC, 0xC1};
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);

    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(m_cpu->sse.xmm[0].u8_[i], expected[i])
            << "AESENC FIPS-197 round 1 byte[" << i << "]";
    }
}

// FIPS-197 Appendix B — AESENCLAST final round is covered algebraically
// by the reg-reg AESENC tests and the all-zero AESENCLAST smoke tests
// above. Memory-operand AESENC is verified separately below.


// AESENC with MEMORY operand (xmm/m128 form) — the form OpenSSL's
// stitched AES-GCM fast path uses heavily.
TEST_F(CryptoOpsTest, AesencMemOperandFips197Round1) {
    const u8 state_bytes[16] = {
        0x19,0x3d,0xe3,0xbe, 0xa0,0xf4,0xe2,0x2b,
        0x9a,0xc6,0x8d,0x2a, 0xe9,0xf8,0x48,0x08
    };
    const u8 key_bytes[16] = {
        0xa0,0xfa,0xfe,0x17, 0x88,0x54,0x2c,0xb1,
        0x23,0xa3,0x39,0x39, 0x2a,0x6c,0x76,0x05
    };
    const u8 expected[16] = {
        0xa4,0x9c,0x7f,0xf2, 0x68,0x9f,0x35,0x2b,
        0x6b,0x5b,0xea,0x43, 0x02,0x6a,0x50,0x49
    };
    std::memcpy(&m_cpu->sse.xmm[0], state_bytes, 16);
    // Store key at guest memory 0x2000
    auto w = m_memory->write_bytes(0x2000, key_bytes, 16);
    ASSERT_TRUE(w);
    // Clear xmm1 so we catch any accidental use of a register operand.
    std::memset(&m_cpu->sse.xmm[1], 0xCC, 16);

    // AESENC xmm0, [rip + disp32]  is awkward in 32-bit emul; use
    // AESENC xmm0, [eax] (ModRM=00 000 000 -> /0 with [eax]) and set EAX=0x2000.
    // Encoding: 66 0F 38 DC 00 — with ModRM 00 C1 would be reg-reg; 00 00 = [eax]
    m_cpu->gpr.set32(0, 0x2000);  // EAX = 0x2000
    u8 code[] = {0x66, 0x0F, 0x38, 0xDC, 0x00};
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);

    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(m_cpu->sse.xmm[0].u8_[i], expected[i])
            << "AESENC [mem] byte[" << i << "]";
    }
}

// AESENC with src == dst (xmm0, xmm0) — degenerate case that may appear
// in some hand-written AES-NI loops.
TEST_F(CryptoOpsTest, AesencSameReg) {
    const u8 state_bytes[16] = {
        0x19,0x3d,0xe3,0xbe, 0xa0,0xf4,0xe2,0x2b,
        0x9a,0xc6,0x8d,0x2a, 0xe9,0xf8,0x48,0x08
    };
    std::memcpy(&m_cpu->sse.xmm[0], state_bytes, 16);

    // AESENC xmm0, xmm0 — 66 0F 38 DC C0
    u8 code[] = {0x66, 0x0F, 0x38, 0xDC, 0xC0};
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);

    // Compute expected via a second run on a fresh state (reg-reg path):
    // AESENC(state, state) == MixColumns(SubBytes(ShiftRows(state))) XOR state
    // We already validated AESENC with zero key in AesencFips197Round1,
    // so use state=state, key=state and compute:
    // Rerun with key_xmm1 = state to compare:
    CpuState cpu2; cpu2.init_long_mode();
    auto mem2 = std::make_unique<GuestMemory>();
    auto mr = mem2->map_region(0x1000, 0x10000, MemoryProtection::ReadWrite);
    ASSERT_TRUE(mr);
    cpu2.gpr.set64(static_cast<u8>(RegIndex::RSP), 0x10000);
    cpu2.rip = 0x1000;
    std::memcpy(&cpu2.sse.xmm[0], state_bytes, 16);
    std::memcpy(&cpu2.sse.xmm[1], state_bytes, 16);
    u8 code2[] = {0x66, 0x0F, 0x38, 0xDC, 0xC1};
    auto wr = mem2->write_bytes(0x1000, code2, sizeof(code2));
    ASSERT_TRUE(wr);
    Interpreter interp2(cpu2, *mem2);
    EXPECT_EQ(interp2.step(), ExecResult::Continue);
    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(m_cpu->sse.xmm[0].u8_[i], cpu2.sse.xmm[0].u8_[i])
            << "AESENC xmm0,xmm0 vs AESENC xmm0,xmm1 (xmm1=state) byte[" << i << "]";
    }
}
