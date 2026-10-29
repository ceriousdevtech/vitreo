// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// SSE data transfer instruction tests (CPU-031)
//
// Integration: add to tests/CMakeLists.txt:
//     cpu/test_sse_data.cpp

#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include <gtest/gtest.h>
#include <array>
#include <cstring>

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

// ============================================================================
// Test Fixture
// ============================================================================

class SseDataTest : public ::testing::Test {
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

    /// Write 16 bytes at given guest address
    void write_xmm_mem(u64 addr, const u8* data) {
        auto result = m_memory->write_bytes(addr, data, 16);
        ASSERT_TRUE(result);
    }

    std::unique_ptr<GuestMemory> m_memory;
    std::unique_ptr<CpuState> m_cpu;
};

// ============================================================================
// Test 1: XORPS xmm0, xmm0 — zeros the register
// ============================================================================
TEST_F(SseDataTest, XorpsSelfZerosRegister) {
    // Fill xmm0 with non-zero data
    for (int i = 0; i < 16; ++i) m_cpu->sse.xmm[0].u8_[i] = 0xFF;

    // 0F 57 C0 — XORPS xmm0, xmm0
    u8 code[] = { 0x0F, 0x57, 0xC0 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);

    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(m_cpu->sse.xmm[0].u8_[i], 0) << "byte " << i;
    }
}

// ============================================================================
// Test 2: MOVAPS reg-to-reg copies full 128 bits
// ============================================================================
TEST_F(SseDataTest, MovapsRegToReg) {
    // Fill xmm1 with all 0xFF, clear xmm0
    for (int i = 0; i < 16; ++i) m_cpu->sse.xmm[1].u8_[i] = 0xFF;
    m_cpu->sse.xmm[0].clear();

    // 0F 28 C1 — MOVAPS xmm0, xmm1
    u8 code[] = { 0x0F, 0x28, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);

    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(m_cpu->sse.xmm[0].u8_[i], 0xFF) << "byte " << i;
    }
}

// ============================================================================
// Test 3: MOVUPS loads from unaligned memory
// ============================================================================
TEST_F(SseDataTest, MovupsFromUnalignedMemory) {
    u8 pattern[16];
    for (int i = 0; i < 16; ++i) pattern[i] = static_cast<u8>(i + 1);
    write_xmm_mem(0x2001, pattern);

    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), 0x2001);

    // 0F 10 03 — MOVUPS xmm0, [rbx]
    u8 code[] = { 0x0F, 0x10, 0x03 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);

    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(m_cpu->sse.xmm[0].u8_[i], static_cast<u8>(i + 1)) << "byte " << i;
    }
}

// ============================================================================
// Test 4: MOVSS load from memory zeros upper bits [127:32]
// ============================================================================
TEST_F(SseDataTest, MovssLoadFromMemZerosHigh) {
    // Pre-fill xmm0 with all 1s so we can verify zeroing
    for (int i = 0; i < 16; ++i) m_cpu->sse.xmm[0].u8_[i] = 0xFF;

    f32 val = 3.14f;
    m_memory->write_bytes(0x2000, reinterpret_cast<const u8*>(&val), 4);
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), 0x2000);

    // F3 0F 10 03 — MOVSS xmm0, [rbx]
    u8 code[] = { 0xF3, 0x0F, 0x10, 0x03 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);

    f32 result;
    std::memcpy(&result, &m_cpu->sse.xmm[0].u32_[0], 4);
    EXPECT_FLOAT_EQ(result, 3.14f);

    // Upper 96 bits must be zero
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[1], 0u);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[2], 0u);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[3], 0u);
}

// ============================================================================
// Test 5: MOVSS reg-to-reg preserves destination bits [127:32]
// ============================================================================
TEST_F(SseDataTest, MovssRegToRegPreservesHigh) {
    // xmm0 all 0xFF
    for (int i = 0; i < 16; ++i) m_cpu->sse.xmm[0].u8_[i] = 0xFF;

    // xmm1: known low dword
    m_cpu->sse.xmm[1].clear();
    m_cpu->sse.xmm[1].u32_[0] = 0x12345678;

    // F3 0F 10 C1 — MOVSS xmm0, xmm1  (reg-to-reg)
    u8 code[] = { 0xF3, 0x0F, 0x10, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);

    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[0], 0x12345678u);
    // Upper dwords must be preserved (still 0xFFFFFFFF)
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[1], 0xFFFFFFFFu);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[2], 0xFFFFFFFFu);
    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[3], 0xFFFFFFFFu);
}

// ============================================================================
// Test 6: MOVSD load from memory zeros upper bits [127:64]
// ============================================================================
TEST_F(SseDataTest, MovsdLoadFromMemZerosHigh) {
    for (int i = 0; i < 16; ++i) m_cpu->sse.xmm[0].u8_[i] = 0xFF;

    f64 val = 2.718281828;
    m_memory->write_bytes(0x2000, reinterpret_cast<const u8*>(&val), 8);
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), 0x2000);

    // F2 0F 10 03 — MOVSD xmm0, [rbx]
    u8 code[] = { 0xF2, 0x0F, 0x10, 0x03 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);

    f64 result;
    std::memcpy(&result, &m_cpu->sse.xmm[0].u64_[0], 8);
    EXPECT_DOUBLE_EQ(result, 2.718281828);

    // Upper 64 bits must be zero
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[1], 0u);
}

// ============================================================================
// Test 7: ANDPS masks correctly
// ============================================================================
TEST_F(SseDataTest, AndpsMasksCorrectly) {
    m_cpu->sse.xmm[0].u64_[0] = 0xFFFFFFFFFFFFFFFFULL;
    m_cpu->sse.xmm[0].u64_[1] = 0xFFFFFFFFFFFFFFFFULL;
    m_cpu->sse.xmm[1].u64_[0] = 0x00FF00FF00FF00FFULL;
    m_cpu->sse.xmm[1].u64_[1] = 0;

    // 0F 54 C1 — ANDPS xmm0, xmm1
    u8 code[] = { 0x0F, 0x54, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);

    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[0], 0x00FF00FF00FF00FFULL);
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[1], 0u);
}

// ============================================================================
// Test 8: ORPS combines correctly
// ============================================================================
TEST_F(SseDataTest, OrpsCombinesCorrectly) {
    m_cpu->sse.xmm[0].u64_[0] = 0xFF00FF00FF00FF00ULL;
    m_cpu->sse.xmm[0].u64_[1] = 0;
    m_cpu->sse.xmm[1].u64_[0] = 0x00FF00FF00FF00FFULL;
    m_cpu->sse.xmm[1].u64_[1] = 0xAAAAAAAAAAAAAAAAULL;

    // 0F 56 C1 — ORPS xmm0, xmm1
    u8 code[] = { 0x0F, 0x56, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);

    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[0], 0xFFFFFFFFFFFFFFFFULL);
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[1], 0xAAAAAAAAAAAAAAAAULL);
}

// ============================================================================
// Test 9: MOVDQA aligned load
// ============================================================================
TEST_F(SseDataTest, MovdqaAlignedLoad) {
    u8 pattern[16];
    for (int i = 0; i < 16; ++i) pattern[i] = static_cast<u8>(0xA0 + i);
    write_xmm_mem(0x2000, pattern);  // 0x2000 is 16-byte aligned

    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), 0x2000);

    // 66 0F 6F 03 — MOVDQA xmm0, [rbx]
    u8 code[] = { 0x66, 0x0F, 0x6F, 0x03 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);

    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(m_cpu->sse.xmm[0].u8_[i], static_cast<u8>(0xA0 + i)) << "byte " << i;
    }
}

// ============================================================================
// Test 10: MOVDQU unaligned load
// ============================================================================
TEST_F(SseDataTest, MovdquUnalignedLoad) {
    u8 pattern[16];
    for (int i = 0; i < 16; ++i) pattern[i] = static_cast<u8>(0xB0 + i);
    write_xmm_mem(0x2003, pattern);

    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), 0x2003);

    // F3 0F 6F 03 — MOVDQU xmm0, [rbx]
    u8 code[] = { 0xF3, 0x0F, 0x6F, 0x03 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);

    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(m_cpu->sse.xmm[0].u8_[i], static_cast<u8>(0xB0 + i)) << "byte " << i;
    }
}

// ============================================================================
// Test 10b: LDDQU unaligned load (SSE3; Spotify/CEF renderer)
// Intel SDM Vol. 2: F2 0F F0 /r — same architectural result as MOVDQU load.
// ============================================================================
TEST_F(SseDataTest, LddquUnalignedLoad) {
    u8 pattern[16];
    for (int i = 0; i < 16; ++i) pattern[i] = static_cast<u8>(0xC0 + i);
    write_xmm_mem(0x2005, pattern);

    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), 0x2005);

    // F2 0F F0 03 — LDDQU xmm0, [rbx]
    u8 code[] = { 0xF2, 0x0F, 0xF0, 0x03 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);

    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(m_cpu->sse.xmm[0].u8_[i], static_cast<u8>(0xC0 + i)) << "byte " << i;
    }
}

// ============================================================================
// Test 11: MOVMSKPS extracts sign bits of 4 single-precision floats
// ============================================================================
TEST_F(SseDataTest, MovmskpsExtractsSignBits) {
    m_cpu->sse.xmm[1].f32_[0] = -1.0f;   // sign bit set  → bit 0
    m_cpu->sse.xmm[1].f32_[1] =  1.0f;   // positive      → bit 1 clear
    m_cpu->sse.xmm[1].f32_[2] = -2.5f;   // sign bit set  → bit 2
    m_cpu->sse.xmm[1].f32_[3] =  0.0f;   // positive zero → bit 3 clear

    // 0F 50 C1 — MOVMSKPS eax, xmm1
    u8 code[] = { 0x0F, 0x50, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);

    // bits 0,2 set → 0b0101 = 5
    EXPECT_EQ(m_cpu->gpr.get32(static_cast<u8>(RegIndex::RAX)), 5u);
}

// ============================================================================
// Test 12: LDMXCSR / STMXCSR round-trip
// ============================================================================
TEST_F(SseDataTest, LdmxcsrStmxcsrRoundTrip) {
    u32 mxcsr_val = 0x1FA0;  // Rounding mode toward zero + all exceptions masked
    m_memory->write_u32(0x2000, mxcsr_val);
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), 0x2000);

    // 0F AE 13 — LDMXCSR [rbx]  (modrm: mod=0, reg=2, rm=3=rbx)
    u8 code_ld[] = { 0x0F, 0xAE, 0x13 };
    write_code(code_ld, sizeof(code_ld));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.mxcsr, mxcsr_val & m_cpu->sse.mxcsr_mask);

    // Now STMXCSR to a different location
    m_cpu->rip = 0x1000;
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), 0x2010);

    // 0F AE 1B — STMXCSR [rbx]  (modrm: mod=0, reg=3, rm=3=rbx)
    u8 code_st[] = { 0x0F, 0xAE, 0x1B };
    write_code(code_st, sizeof(code_st));

    EXPECT_EQ(execute_one(), ExecResult::Continue);

    auto stored = m_memory->read_u32(0x2010);
    ASSERT_TRUE(stored);
    EXPECT_EQ(stored.value(), m_cpu->sse.mxcsr);
}

TEST_F(SseDataTest, FxsaveWritesLegacyStateImageInProtectedMode) {
    m_cpu->init_protected_mode();
    m_cpu->rip = 0x1000;
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RBX), 0x2000);
    m_cpu->fpu.control_word = 0x1234;
    m_cpu->fpu.status_word = 0x3001; // TOP=6
    m_cpu->fpu.tag_word = 0x8FFF;    // Physical R6/R7 are non-empty
    m_cpu->fpu.fop = 0xF456;
    m_cpu->fpu.fip = 0x11223344;
    m_cpu->fpu.fdp = 0x55667788;
    m_cpu->fpu.st[6] = Float80(3.141592653589793);
    m_cpu->fpu.st[7] = Float80(1.0);
    m_cpu->sse.mxcsr = 0x1FA0;
    for (u8 i = 0; i < 16; ++i) {
        m_cpu->sse.xmm[0].u8_[i] = static_cast<u8>(0xA0 + i);
        m_cpu->sse.xmm[7].u8_[i] = static_cast<u8>(0x70 + i);
        m_cpu->sse.xmm[8].u8_[i] = 0xEE;
    }

    const u8 code[] = { 0x0F, 0xAE, 0x03 }; // FXSAVE [ebx]
    write_code(code, sizeof(code));
    ASSERT_EQ(execute_one(), ExecResult::Continue);

    std::array<u8, 512> image{};
    ASSERT_TRUE(m_memory->read_bytes(0x2000, image.data(), image.size()));
    auto load_u16 = [&image](usize offset) {
        u16 value;
        std::memcpy(&value, image.data() + offset, sizeof(value));
        return value;
    };
    auto load_u32 = [&image](usize offset) {
        u32 value;
        std::memcpy(&value, image.data() + offset, sizeof(value));
        return value;
    };

    EXPECT_EQ(load_u16(0), 0x1234);
    EXPECT_EQ(load_u16(2), 0x3001);
    EXPECT_EQ(image[4], 0xC0);
    EXPECT_EQ(load_u16(6), 0x0456);
    EXPECT_EQ(load_u32(8), 0x11223344u);
    EXPECT_EQ(load_u32(16), 0x55667788u);
    EXPECT_EQ(load_u32(24), 0x1FA0u);
    EXPECT_EQ(load_u32(28), m_cpu->sse.mxcsr_mask);
    EXPECT_EQ(0, std::memcmp(image.data() + 32, &m_cpu->fpu.st[6], 10));
    EXPECT_EQ(0, std::memcmp(image.data() + 48, &m_cpu->fpu.st[7], 10));
    EXPECT_EQ(0, std::memcmp(image.data() + 160,
                             m_cpu->sse.xmm[0].u8_, 16));
    EXPECT_EQ(0, std::memcmp(image.data() + 272,
                             m_cpu->sse.xmm[7].u8_, 16));
    for (usize i = 288; i < 416; ++i) {
        EXPECT_EQ(image[i], 0) << "reserved x86 byte " << i;
    }
}

// MOVSHDUP/MOVSLDUP take a 128-bit memory operand and require it 16-byte
// aligned, like every other legacy SSE form that is not one of the explicitly
// unaligned spellings.
//
// Measured, not reasoned. These two were previously exempt in BOTH engines --
// the interpreter read them through the unchecked loader and the JIT listed
// them as exclusions -- with a comment saying they were unmeasured and asking
// for an oracle run before changing anything. The run says they fault:
// `movshdup xmm0,[eax]` and `movsldup xmm0,[eax]` with the operand misaligned
// by one both report FAULT code=c0000005 on real silicon (Windows
// 10.0.26200), recorded in tools/oracle_probe/sse_align_probe.expected.
//
// Two engines agreeing with each other is not evidence either is right: this
// is the shape parity testing structurally cannot see, so the oracle is the
// only thing that settles it.
TEST_F(SseDataTest, MovsdupFormsRequireSixteenByteAlignment) {
    struct Form { const char* name; u8 prefix; u8 op; };
    for (const Form form : {Form{"movsldup", 0xF3, 0x12},
                            Form{"movshdup", 0xF3, 0x16}}) {
        SCOPED_TRACE(form.name);

        // Misaligned by one: raises, and leaves the destination alone.
        m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), 0x2001);
        m_cpu->sse.xmm[0] = {};
        const u8 bad[] = { form.prefix, 0x0F, form.op, 0x03 }; // op xmm0,[rbx]
        write_code(bad, sizeof(bad));
        m_cpu->rip = 0x1000;
        EXPECT_EQ(execute_one(), ExecResult::Exception) << "misaligned by 1";

        // The same operand at a 16-byte boundary completes. A test that only
        // asserted the fault would also pass against an implementation that
        // rejected every address.
        m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), 0x2000);
        m_cpu->rip = 0x1000;
        EXPECT_EQ(execute_one(), ExecResult::Continue) << "16-byte aligned";
    }
}

TEST_F(SseDataTest, FxsaveRequiresSixteenByteAlignment) {
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), 0x2001);
    const u8 code[] = { 0x0F, 0xAE, 0x03 }; // FXSAVE [rbx]
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Exception);
}

// ============================================================================
// Test 13: MOVLPS loads low qword, preserves high qword
// ============================================================================
TEST_F(SseDataTest, MovlpsPartialLoad) {
    m_cpu->sse.xmm[0].u64_[0] = 0x1111111111111111ULL;
    m_cpu->sse.xmm[0].u64_[1] = 0xAAAAAAAAAAAAAAAAULL;

    u64 mem_val = 0xDEADBEEFCAFEBABEULL;
    m_memory->write_u64(0x2000, mem_val);
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), 0x2000);

    // 0F 12 03 — MOVLPS xmm0, [rbx]
    u8 code[] = { 0x0F, 0x12, 0x03 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);

    // Low qword replaced
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[0], 0xDEADBEEFCAFEBABEULL);
    // High qword preserved
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[1], 0xAAAAAAAAAAAAAAAAULL);
}

// ============================================================================
// Test 14: MOVAPS from unaligned address triggers #GP exception
// ============================================================================
TEST_F(SseDataTest, MovapsUnalignedTriggersException) {
    u8 data[16] = {};
    write_xmm_mem(0x2001, data);

    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), 0x2001);

    // 0F 28 03 — MOVAPS xmm0, [rbx]
    u8 code[] = { 0x0F, 0x28, 0x03 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Exception);
}

// ============================================================================
// Test 15: ANDNPS bitwise AND-NOT produces ~dst & src
// ============================================================================
TEST_F(SseDataTest, AndnpsCorrectResult) {
    m_cpu->sse.xmm[0].u64_[0] = 0xFF00FF00FF00FF00ULL;
    m_cpu->sse.xmm[0].u64_[1] = 0xFF00FF00FF00FF00ULL;
    m_cpu->sse.xmm[1].u64_[0] = 0xFFFFFFFFFFFFFFFFULL;
    m_cpu->sse.xmm[1].u64_[1] = 0xFFFFFFFFFFFFFFFFULL;

    // 0F 55 C1 — ANDNPS xmm0, xmm1
    u8 code[] = { 0x0F, 0x55, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);

    // ~0xFF00FF00FF00FF00 = 0x00FF00FF00FF00FF
    // 0x00FF00FF00FF00FF & 0xFFFFFFFFFFFFFFFF = 0x00FF00FF00FF00FF
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[0], 0x00FF00FF00FF00FFULL);
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[1], 0x00FF00FF00FF00FFULL);
}

// ============================================================================
// Test 16: MOVHPS loads high qword, preserves low qword
// ============================================================================
TEST_F(SseDataTest, MovhpsPartialLoad) {
    m_cpu->sse.xmm[0].u64_[0] = 0xBBBBBBBBBBBBBBBBULL;
    m_cpu->sse.xmm[0].u64_[1] = 0;

    u64 mem_val = 0x1234567890ABCDEFULL;
    m_memory->write_u64(0x2000, mem_val);
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), 0x2000);

    // 0F 16 03 — MOVHPS xmm0, [rbx]
    u8 code[] = { 0x0F, 0x16, 0x03 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);

    // Low qword preserved
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[0], 0xBBBBBBBBBBBBBBBBULL);
    // High qword from memory
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[1], 0x1234567890ABCDEFULL);
}

// ============================================================================
// Test 17: MOVMSKPD extracts sign bits of 2 doubles
// ============================================================================
TEST_F(SseDataTest, MovmskpdExtractsSignBits) {
    m_cpu->sse.xmm[2].f64_[0] = -1.0;  // sign bit set → bit 0
    m_cpu->sse.xmm[2].f64_[1] = -2.0;  // sign bit set → bit 1

    // 66 0F 50 C2 — MOVMSKPD eax, xmm2
    u8 code[] = { 0x66, 0x0F, 0x50, 0xC2 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);

    // Both sign bits set → 0b11 = 3
    EXPECT_EQ(m_cpu->gpr.get32(static_cast<u8>(RegIndex::RAX)), 3u);
}

// ============================================================================
// MOVNTPS m128, xmm — aligned store (non-temporal hint advisory)
// ============================================================================
TEST_F(SseDataTest, MovntpsAlignedStore) {
    for (int i = 0; i < 16; ++i) {
        m_cpu->sse.xmm[0].u8_[i] = static_cast<u8>(0xA0 + i);
    }
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RCX), 0x2000);

    // 0F 2B 01 — MOVNTPS [rcx], xmm0  (Steam/SDL login path)
    u8 code[] = { 0x0F, 0x2B, 0x01 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);

    u8 out[16]{};
    ASSERT_TRUE(m_memory->read_bytes(0x2000, out, 16));
    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(out[i], static_cast<u8>(0xA0 + i)) << "byte " << i;
    }
}

TEST_F(SseDataTest, MovntpsMisalignedRaisesGp) {
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RCX), 0x2001);
    u8 code[] = { 0x0F, 0x2B, 0x01 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Exception);
}

TEST_F(SseDataTest, MovntpdAlignedStore) {
    m_cpu->sse.xmm[1].u64_[0] = 0x1111111111111111ULL;
    m_cpu->sse.xmm[1].u64_[1] = 0x2222222222222222ULL;
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RAX), 0x2010);

    // 66 0F 2B 08 — MOVNTPD [rax], xmm1
    u8 code[] = { 0x66, 0x0F, 0x2B, 0x08 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);

    EXPECT_EQ(m_memory->read_u64(0x2010).value(), 0x1111111111111111ULL);
    EXPECT_EQ(m_memory->read_u64(0x2018).value(), 0x2222222222222222ULL);
}

TEST_F(SseDataTest, MovntdqAlignedStore) {
    for (int i = 0; i < 16; ++i)
        m_cpu->sse.xmm[0].u8_[i] = static_cast<u8>(0xB0 + i);
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RCX), 0x2010);

    u8 code[] = { 0x66, 0x0F, 0xE7, 0x01 };  // MOVNTDQ [rcx], xmm0
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);

    u8 out[16]{};
    ASSERT_TRUE(m_memory->read_bytes(0x2010, out, 16));
    for (int i = 0; i < 16; ++i)
        EXPECT_EQ(out[i], static_cast<u8>(0xB0 + i)) << "byte " << i;
}

TEST_F(SseDataTest, MaskmovdquStoresSelectedBytesToDsRdi) {
    for (int i = 0; i < 16; ++i) {
        m_cpu->sse.xmm[0].u8_[i] = static_cast<u8>(i + 1);
        m_cpu->sse.xmm[1].u8_[i] = (i & 1) ? 0x00 : 0x80;
    }
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RDI), 0x2000);
    for (u64 i = 0; i < 16; ++i)
        ASSERT_TRUE(m_memory->write_u8(0x2000 + i, 0xFF));

    u8 code[] = { 0x66, 0x0F, 0xF7, 0xC1 };  // MASKMOVDQU xmm0, xmm1
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);

    for (int i = 0; i < 16; ++i) {
        const u8 expected = (i & 1) ? 0xFF : static_cast<u8>(i + 1);
        EXPECT_EQ(m_memory->read_u8(0x2000 + static_cast<u64>(i)).value(), expected)
            << "byte " << i;
    }
    EXPECT_EQ(m_cpu->gpr.get64(static_cast<u8>(RegIndex::RDI)), 0x2000ull);
}

TEST_F(SseDataTest, DecodeFxrstorM512) {
    const u8 code[] = { 0x0F, 0xAE, 0x09 }; // FXRSTOR [rcx]
    MemoryByteReader reader(code, sizeof(code));
    DecodedInstruction inst;
    ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode64, inst),
              DecodeError::Success);
    EXPECT_EQ(inst.opcode, Opcode::FXRSTOR_M512);
}

TEST_F(SseDataTest, MovntiStoresDword) {
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RCX), 0x2000);
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RAX), 0xA1B2C3D4u);

    const u8 code[] = { 0x0F, 0xC3, 0x01 }; // MOVNTI [rcx], eax
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_memory->read_u32(0x2000).value(), 0xA1B2C3D4u);
}

TEST_F(SseDataTest, ClflushContinues) {
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RCX), 0x2000);

    const u8 code[] = { 0x0F, 0xAE, 0x39 }; // CLFLUSH [rcx]
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
}

TEST_F(SseDataTest, FxsaveFxrstorRoundTrip) {
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RCX), 0x2010);
    m_cpu->fpu.control_word = 0x027F;
    for (int i = 0; i < 16; ++i) {
        m_cpu->sse.xmm[0].u8_[i] = static_cast<u8>(0xA0 + i);
    }

    const u8 code[] = {
        0x0F, 0xAE, 0x01, // FXSAVE [rcx]
        0x0F, 0xAE, 0x09, // FXRSTOR [rcx]
    };
    write_code(code, sizeof(code));

    ASSERT_EQ(execute_one(), ExecResult::Continue);

    m_cpu->fpu.control_word = 0x037F;
    for (int i = 0; i < 16; ++i) {
        m_cpu->sse.xmm[0].u8_[i] = 0xFF;
    }

    ASSERT_EQ(execute_one(), ExecResult::Continue);

    EXPECT_EQ(m_cpu->fpu.control_word, 0x027F);
    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(m_cpu->sse.xmm[0].u8_[i], static_cast<u8>(0xA0 + i)) << "byte " << i;
    }
}

namespace {

BasicBlock decode_sse_block(GuestMemory& memory, const u8* code, usize len,
                            bool is_64bit) {
    EXPECT_TRUE(memory.write_bytes(0x1000, code, len));

    BasicBlock block;
    block.start_addr = 0x1000;
    block.is_64bit = is_64bit;

    MemoryByteReader reader(code, len);
    const DecodeMode mode = is_64bit ? DecodeMode::Mode64 : DecodeMode::Mode32;
    while (reader.remaining() > 0) {
        DecodedInstruction inst;
        if (decode_instruction(reader, mode, inst) != DecodeError::Success) {
            break;
        }
        block.instructions.push_back(inst);
    }
    block.byte_length = static_cast<u32>(reader.position());
    block.instruction_count = static_cast<u32>(block.instructions.size());
    return block;
}

} // namespace

TEST_F(SseDataTest, JitClflushAdvancesRip64) {
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RCX), 0x2000);
    const u8 code[] = {0x0F, 0xAE, 0x39};  // CLFLUSH [rcx]
    BasicBlock block = decode_sse_block(*m_memory, code, sizeof(code), true);
    ASSERT_EQ(block.instruction_count, 1u);

    JitCompiler jit(*m_memory);
    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr);
    EXPECT_EQ(compiled->native_insns, 1u);
    EXPECT_EQ(jit.execute(*compiled, *m_cpu), JitExecResult::Continue);
    EXPECT_FALSE(m_cpu->pending_raise.active);
    EXPECT_EQ(m_cpu->rip, 0x1000ull + sizeof(code));
}

TEST_F(SseDataTest, JitClflushAdvancesRip32) {
    m_cpu->init_protected_mode();
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RBX), 0x2000);
    m_cpu->rip = 0x1000;
    const u8 code[] = {0x0F, 0xAE, 0x3B};  // CLFLUSH [ebx]
    BasicBlock block = decode_sse_block(*m_memory, code, sizeof(code), false);
    ASSERT_EQ(block.instruction_count, 1u);

    JitCompiler jit(*m_memory);
    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr);
    EXPECT_EQ(compiled->native_insns, 1u);
    EXPECT_EQ(jit.execute(*compiled, *m_cpu), JitExecResult::Continue);
    EXPECT_FALSE(m_cpu->pending_raise.active);
    EXPECT_EQ(m_cpu->rip, 0x1000ull + sizeof(code));
}

TEST_F(SseDataTest, JitMovntiStoresDword) {
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RCX), 0x2000);
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RAX), 0xA1B2C3D4u);
    const u8 code[] = {0x0F, 0xC3, 0x01};  // MOVNTI [rcx], eax
    BasicBlock block = decode_sse_block(*m_memory, code, sizeof(code), true);
    ASSERT_EQ(block.instruction_count, 1u);

    JitCompiler jit(*m_memory);
    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr);
    EXPECT_EQ(compiled->native_insns, 1u);
    EXPECT_EQ(jit.execute(*compiled, *m_cpu), JitExecResult::Continue);
    EXPECT_FALSE(m_cpu->pending_raise.active);
    EXPECT_EQ(m_memory->read_u32(0x2000).value(), 0xA1B2C3D4u);
    EXPECT_EQ(m_cpu->rip, 0x1000ull + sizeof(code));
}

TEST_F(SseDataTest, JitMovntiStoresQword) {
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RCX), 0x2000);
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RAX), 0x1122334455667788ull);
    const u8 code[] = {0x48, 0x0F, 0xC3, 0x01};  // MOVNTI [rcx], rax
    BasicBlock block = decode_sse_block(*m_memory, code, sizeof(code), true);
    ASSERT_EQ(block.instruction_count, 1u);

    JitCompiler jit(*m_memory);
    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr);
    EXPECT_EQ(compiled->native_insns, 1u);
    EXPECT_EQ(jit.execute(*compiled, *m_cpu), JitExecResult::Continue);
    EXPECT_FALSE(m_cpu->pending_raise.active);
    EXPECT_EQ(m_memory->read_u64(0x2000).value(), 0x1122334455667788ull);
    EXPECT_EQ(m_cpu->rip, 0x1000ull + sizeof(code));
}

TEST_F(SseDataTest, JitFxsaveMatchesInterpreterImage) {
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), 0x2000);
    m_cpu->fpu.control_word = 0x1234;
    m_cpu->fpu.status_word = 0x3001;
    m_cpu->fpu.tag_word = 0x8FFF;
    m_cpu->fpu.fop = 0xF456;
    m_cpu->fpu.fip = 0x11223344;
    m_cpu->fpu.fdp = 0x55667788;
    m_cpu->fpu.st[6] = Float80(3.141592653589793);
    m_cpu->fpu.st[7] = Float80(1.0);
    m_cpu->sse.mxcsr = 0x1FA0;
    for (u8 i = 0; i < 16; ++i) {
        m_cpu->sse.xmm[0].u8_[i] = static_cast<u8>(0xA0 + i);
        m_cpu->sse.xmm[7].u8_[i] = static_cast<u8>(0x70 + i);
        m_cpu->sse.xmm[8].u8_[i] = 0xEE;
    }

    const u8 code[] = {0x0F, 0xAE, 0x03};  // FXSAVE [rbx]
    write_code(code, sizeof(code));

    CpuState interp_cpu = *m_cpu;
    Interpreter interp(interp_cpu, *m_memory);
    ASSERT_EQ(interp.step(), ExecResult::Continue);

    std::array<u8, 512> interp_image{};
    ASSERT_TRUE(m_memory->read_bytes(0x2000, interp_image.data(),
                                     interp_image.size()));

    std::array<u8, 512> zeros{};
    ASSERT_TRUE(m_memory->write_bytes(0x2000, zeros.data(), zeros.size()));

    BasicBlock block = decode_sse_block(*m_memory, code, sizeof(code), true);
    ASSERT_EQ(block.instruction_count, 1u);
    JitCompiler jit(*m_memory);
    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr);
    EXPECT_EQ(compiled->native_insns, 1u);
    EXPECT_EQ(jit.execute(*compiled, *m_cpu), JitExecResult::Continue);
    EXPECT_FALSE(m_cpu->pending_raise.active);

    std::array<u8, 512> jit_image{};
    ASSERT_TRUE(m_memory->read_bytes(0x2000, jit_image.data(), jit_image.size()));
    EXPECT_EQ(0, std::memcmp(interp_image.data(), jit_image.data(), 512));
    EXPECT_EQ(m_cpu->rip, interp_cpu.rip);
}

TEST_F(SseDataTest, JitFxsaveUnalignedRaises) {
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), 0x2001);
    const u8 code[] = {0x0F, 0xAE, 0x03};  // FXSAVE [rbx]
    BasicBlock block = decode_sse_block(*m_memory, code, sizeof(code), true);
    ASSERT_EQ(block.instruction_count, 1u);

    JitCompiler jit(*m_memory);
    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr);
    EXPECT_EQ(compiled->native_insns, 1u);
    EXPECT_EQ(jit.execute(*compiled, *m_cpu), JitExecResult::Continue);
    EXPECT_TRUE(m_cpu->pending_raise.active);
    EXPECT_EQ(m_cpu->pending_raise.code, 0xC0000005u);
}

// Intel SDM Vol. 2 FXSAVE: in 64-bit mode REX.W selects the FXSAVE64 map
// (64-bit FIP at offset 8, 64-bit FDP at offset 16). Without REX.W the
// 32-bit FIP/FDP fields occupy those slots and FCS/FDS sit after them.
TEST_F(SseDataTest, JitFxsave64MatchesInterpreterImage) {
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), 0x2000);
    m_cpu->fpu.control_word = 0x1234;
    m_cpu->fpu.status_word = 0x3001;
    m_cpu->fpu.tag_word = 0x8FFF;
    m_cpu->fpu.fop = 0xF456;
    m_cpu->fpu.fip = 0xAABBCCDD11223344ull;
    m_cpu->fpu.fdp = 0xEEFF001155667788ull;
    m_cpu->fpu.st[6] = Float80(3.141592653589793);
    m_cpu->fpu.st[7] = Float80(1.0);
    m_cpu->sse.mxcsr = 0x1FA0;
    for (u8 i = 0; i < 16; ++i) {
        m_cpu->sse.xmm[0].u8_[i] = static_cast<u8>(0xA0 + i);
        m_cpu->sse.xmm[7].u8_[i] = static_cast<u8>(0x70 + i);
        m_cpu->sse.xmm[8].u8_[i] = 0xEE;
    }

    const CpuState snap = *m_cpu;
    const u8 fxsave[] = {0x0F, 0xAE, 0x03};        // FXSAVE [rbx]
    const u8 fxsave64[] = {0x48, 0x0F, 0xAE, 0x03}; // FXSAVE64 [rbx]
    std::array<u8, 512> zeros{};

    write_code(fxsave, sizeof(fxsave));
    CpuState interp32 = snap;
    Interpreter i32(interp32, *m_memory);
    ASSERT_EQ(i32.step(), ExecResult::Continue);
    std::array<u8, 512> image32{};
    ASSERT_TRUE(m_memory->read_bytes(0x2000, image32.data(), image32.size()));

    ASSERT_TRUE(m_memory->write_bytes(0x2000, zeros.data(), zeros.size()));
    write_code(fxsave64, sizeof(fxsave64));
    CpuState interp64 = snap;
    interp64.rip = 0x1000;
    Interpreter i64(interp64, *m_memory);
    ASSERT_EQ(i64.step(), ExecResult::Continue);
    std::array<u8, 512> image64{};
    ASSERT_TRUE(m_memory->read_bytes(0x2000, image64.data(), image64.size()));

    u32 fip32 = 0, fdp32 = 0;
    u64 fip64 = 0, fdp64 = 0;
    std::memcpy(&fip32, image32.data() + 8, sizeof(fip32));
    std::memcpy(&fdp32, image32.data() + 16, sizeof(fdp32));
    std::memcpy(&fip64, image64.data() + 8, sizeof(fip64));
    std::memcpy(&fdp64, image64.data() + 16, sizeof(fdp64));
    EXPECT_EQ(fip32, 0x11223344u);
    EXPECT_EQ(fdp32, 0x55667788u);
    EXPECT_EQ(fip64, 0xAABBCCDD11223344ull);
    EXPECT_EQ(fdp64, 0xEEFF001155667788ull);
    EXPECT_NE(0, std::memcmp(image32.data() + 8, image64.data() + 8, 16));
    EXPECT_EQ(0, std::memcmp(image32.data(), image64.data(), 8));
    EXPECT_EQ(0, std::memcmp(image32.data() + 24, image64.data() + 24, 512 - 24));

    ASSERT_TRUE(m_memory->write_bytes(0x2000, zeros.data(), zeros.size()));
    *m_cpu = snap;
    m_cpu->rip = 0x1000;
    BasicBlock save_block = decode_sse_block(*m_memory, fxsave64, sizeof(fxsave64),
                                             true);
    ASSERT_EQ(save_block.instruction_count, 1u);
    JitCompiler jit(*m_memory);
    CompiledBlock* saved = jit.compile(save_block);
    ASSERT_NE(saved, nullptr);
    EXPECT_EQ(saved->native_insns, 1u);
    EXPECT_EQ(jit.execute(*saved, *m_cpu), JitExecResult::Continue);
    EXPECT_FALSE(m_cpu->pending_raise.active);
    std::array<u8, 512> jit_image{};
    ASSERT_TRUE(m_memory->read_bytes(0x2000, jit_image.data(), jit_image.size()));
    EXPECT_EQ(0, std::memcmp(image64.data(), jit_image.data(), 512));
    EXPECT_EQ(m_cpu->rip, interp64.rip);

    m_cpu->fpu.control_word = 0x037F;
    m_cpu->fpu.status_word = 0;
    m_cpu->fpu.fip = 0;
    m_cpu->fpu.fdp = 0;
    m_cpu->sse.mxcsr = 0x1F80;
    for (u8 i = 0; i < 16; ++i) {
        m_cpu->sse.xmm[0].u8_[i] = 0;
        m_cpu->sse.xmm[7].u8_[i] = 0;
        m_cpu->sse.xmm[8].u8_[i] = 0;
    }
    m_cpu->rip = 0x1000;
    const u8 fxrstor64[] = {0x48, 0x0F, 0xAE, 0x0B};  // FXRSTOR64 [rbx]
    BasicBlock rst_block = decode_sse_block(*m_memory, fxrstor64, sizeof(fxrstor64),
                                            true);
    ASSERT_EQ(rst_block.instruction_count, 1u);
    CompiledBlock* restored = jit.compile(rst_block);
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->native_insns, 1u);
    EXPECT_EQ(jit.execute(*restored, *m_cpu), JitExecResult::Continue);
    EXPECT_FALSE(m_cpu->pending_raise.active);
    EXPECT_EQ(m_cpu->fpu.fip, snap.fpu.fip);
    EXPECT_EQ(m_cpu->fpu.fdp, snap.fpu.fdp);
    // FXRSTOR forces the control word's reserved bits exactly as FLDCW does:
    // bit 6 reads back set, bit 7 and bits 13-15 clear. Measured on real
    // silicon (the `Windows` host, 2026-08-30) -- FXRSTOR of a control word
    // 0x1234 reads back 0x1274, 0xFFFF reads back 0x1F7F, 0x0000 reads back
    // 0x0040. So the round-trip is not the identity, and comparing against the
    // raw snapshot asserted a control word no CPU can produce.
    EXPECT_EQ(m_cpu->fpu.control_word,
              FpuState::sanitize_control_word(snap.fpu.control_word));
    EXPECT_EQ(m_cpu->sse.mxcsr, snap.sse.mxcsr);
    for (u8 i = 0; i < 16; ++i) {
        EXPECT_EQ(m_cpu->sse.xmm[0].u8_[i], snap.sse.xmm[0].u8_[i]) << "xmm0 " << i;
        EXPECT_EQ(m_cpu->sse.xmm[7].u8_[i], snap.sse.xmm[7].u8_[i]) << "xmm7 " << i;
        EXPECT_EQ(m_cpu->sse.xmm[8].u8_[i], snap.sse.xmm[8].u8_[i]) << "xmm8 " << i;
    }
    EXPECT_EQ(m_cpu->rip, 0x1000ull + sizeof(fxrstor64));
}
