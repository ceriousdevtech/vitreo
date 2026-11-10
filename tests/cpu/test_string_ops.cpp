// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// String instruction tests - CPU-024

#include <gtest/gtest.h>
#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"

using namespace vitreo;
using namespace vitreo::cpu;

// ============================================================================
// Decode Helpers
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
// Decoder Tests - String Operations
// ============================================================================

TEST(StringOpsDecode, Movsb) {
    // A4 - MOVSB
    auto inst = decode({0xA4});
    EXPECT_EQ(inst.opcode, Opcode::MOVSB);
    EXPECT_EQ(inst.length, 1);
    EXPECT_EQ(inst.operand_count, 0);
}

TEST(StringOpsDecode, RepMovsb) {
    // F3 A4 - REP MOVSB
    auto inst = decode({0xF3, 0xA4});
    EXPECT_EQ(inst.opcode, Opcode::MOVSB);
    EXPECT_TRUE(inst.prefixes.rep);
    EXPECT_EQ(inst.length, 2);
}

TEST(StringOpsDecode, Movsw) {
    // 66 A5 - MOVSW
    auto inst = decode({0x66, 0xA5});
    EXPECT_EQ(inst.opcode, Opcode::MOVSW);
}

TEST(StringOpsDecode, Movsd) {
    // A5 - MOVSD (default 32-bit operand in 64-bit mode)
    auto inst = decode({0xA5});
    EXPECT_EQ(inst.opcode, Opcode::MOVSD_STR);
}

TEST(StringOpsDecode, Movsq) {
    // 48 A5 - MOVSQ (REX.W)
    auto inst = decode({0x48, 0xA5});
    EXPECT_EQ(inst.opcode, Opcode::MOVSQ);
}

TEST(StringOpsDecode, RepMovsq) {
    // F3 48 A5 - REP MOVSQ
    auto inst = decode({0xF3, 0x48, 0xA5});
    EXPECT_EQ(inst.opcode, Opcode::MOVSQ);
    EXPECT_TRUE(inst.prefixes.rep);
}

TEST(StringOpsDecode, Stosb) {
    // AA - STOSB
    auto inst = decode({0xAA});
    EXPECT_EQ(inst.opcode, Opcode::STOSB);
    EXPECT_EQ(inst.length, 1);
}

TEST(StringOpsDecode, RepStosb) {
    // F3 AA - REP STOSB
    auto inst = decode({0xF3, 0xAA});
    EXPECT_EQ(inst.opcode, Opcode::STOSB);
    EXPECT_TRUE(inst.prefixes.rep);
}

TEST(StringOpsDecode, Lodsb) {
    // AC - LODSB
    auto inst = decode({0xAC});
    EXPECT_EQ(inst.opcode, Opcode::LODSB);
    EXPECT_EQ(inst.length, 1);
}

TEST(StringOpsDecode, Cmpsb) {
    // A6 - CMPSB
    auto inst = decode({0xA6});
    EXPECT_EQ(inst.opcode, Opcode::CMPSB);
    EXPECT_EQ(inst.length, 1);
}

TEST(StringOpsDecode, RepeCmpsb) {
    // F3 A6 - REPE CMPSB
    auto inst = decode({0xF3, 0xA6});
    EXPECT_EQ(inst.opcode, Opcode::CMPSB);
    EXPECT_TRUE(inst.prefixes.rep);
}

TEST(StringOpsDecode, Scasb) {
    // AE - SCASB
    auto inst = decode({0xAE});
    EXPECT_EQ(inst.opcode, Opcode::SCASB);
    EXPECT_EQ(inst.length, 1);
}

TEST(StringOpsDecode, RepneScasb) {
    // F2 AE - REPNE SCASB
    auto inst = decode({0xF2, 0xAE});
    EXPECT_EQ(inst.opcode, Opcode::SCASB);
    EXPECT_TRUE(inst.prefixes.repne);
}

// ============================================================================
// Decoder Tests - Flag Operations
// ============================================================================

TEST(StringOpsDecode, Cld) {
    // FC - CLD
    auto inst = decode({0xFC});
    EXPECT_EQ(inst.opcode, Opcode::CLD);
    EXPECT_EQ(inst.length, 1);
}

TEST(StringOpsDecode, Std) {
    // FD - STD
    auto inst = decode({0xFD});
    EXPECT_EQ(inst.opcode, Opcode::STD);
    EXPECT_EQ(inst.length, 1);
}

TEST(StringOpsDecode, Clc) {
    // F8 - CLC
    auto inst = decode({0xF8});
    EXPECT_EQ(inst.opcode, Opcode::CLC);
    EXPECT_EQ(inst.length, 1);
}

TEST(StringOpsDecode, Stc) {
    // F9 - STC
    auto inst = decode({0xF9});
    EXPECT_EQ(inst.opcode, Opcode::STC);
    EXPECT_EQ(inst.length, 1);
}

TEST(StringOpsDecode, Cmc) {
    // F5 - CMC
    auto inst = decode({0xF5});
    EXPECT_EQ(inst.opcode, Opcode::CMC);
    EXPECT_EQ(inst.length, 1);
}

TEST(StringOpsDecode, Sahf) {
    // 9E - SAHF
    auto inst = decode({0x9E});
    EXPECT_EQ(inst.opcode, Opcode::SAHF);
    EXPECT_EQ(inst.length, 1);
}

TEST(StringOpsDecode, Lahf) {
    // 9F - LAHF
    auto inst = decode({0x9F});
    EXPECT_EQ(inst.opcode, Opcode::LAHF);
    EXPECT_EQ(inst.length, 1);
}

// ============================================================================
// Interpreter Tests
// ============================================================================

constexpr u8 RAX = static_cast<u8>(RegIndex::RAX);
constexpr u8 RCX = static_cast<u8>(RegIndex::RCX);
constexpr u8 RSI = static_cast<u8>(RegIndex::RSI);
constexpr u8 RDI = static_cast<u8>(RegIndex::RDI);

class StringOpsTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        // Map a large region for code + data
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

    void write_data(u64 addr, const u8* data, usize len) {
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

TEST_F(StringOpsTest, RepeatCompareFaultRestoresEntryFlagsAndCommitsProgress) {
    // Intel SDM REP; native Windows oracle rep_compare_fault_probe{32,64}
    // confirms entry status 0x895/0xc95 on both first and partial faults.
    const u64 seed = FLAG_CF | FLAG_PF | FLAG_AF | FLAG_SF | FLAG_OF | 2;
    for (bool wide : {false, true}) for (u8 elem : {u8{1}, u8{2}, u8{4}, u8{8}}) {
        if (!wide && elem == 8) continue;
        for (bool repe : {false, true}) for (bool backward : {false, true})
        for (bool partial : {false, true}) for (int side : {0, 1, 2}) {
            SCOPED_TRACE(::testing::Message() << "wide=" << wide << " elem=" << int(elem)
                << " repe=" << repe << " backward=" << backward
                << " partial=" << partial << " side=" << side);
            std::vector<u8> fill(0x10000, 0x55);
            ASSERT_TRUE(m_memory->write_bytes(0x1000, fill.data(), fill.size()));
            if (!repe) {
                const std::vector<u8> other(32, 0x44);
                ASSERT_TRUE(m_memory->write_bytes(0x3FF0, other.data(), other.size()));
            }
            std::vector<u8> code{static_cast<u8>(repe ? 0xF3 : 0xF2)};
            if (elem == 2) code.push_back(0x66);
            if (elem == 8) code.push_back(0x48);
            code.push_back(side == 0 ? (elem == 1 ? 0xAE : 0xAF)
                                    : (elem == 1 ? 0xA6 : 0xA7));
            ASSERT_TRUE(m_memory->write_bytes(0x8000, code.data(), code.size()));
            if (wide) m_cpu->init_long_mode(); else m_cpu->init_protected_mode();
            m_cpu->rip = 0x8000;
            m_cpu->flags.rflags = seed | (backward ? FLAG_DF : 0);
            const u64 bad = backward ? 0x1000 - elem : 0x11000;
            const u64 first = partial ? (backward ? bad + 2 * elem : bad - 2 * elem) : bad;
            m_cpu->gpr.set64(RSI, side == 1 ? first : 0x4000);
            m_cpu->gpr.set64(RDI, side != 1 ? first : 0x4000);
            m_cpu->gpr.set64(RCX, 4);
            m_cpu->gpr.set64(RAX, repe ? 0x5555555555555555ull : 0x4444444444444444ull);
            EXPECT_EQ(execute_one(), ExecResult::Exception);
            EXPECT_EQ(m_cpu->flags.rflags, seed | (backward ? FLAG_DF : 0));
            EXPECT_EQ(m_cpu->gpr.get64(RCX), partial ? 2u : 4u);
            EXPECT_EQ(m_cpu->gpr.get64(side == 1 ? RSI : RDI), bad);
            EXPECT_EQ(m_cpu->rip, 0x8000u);
        }
    }
}

TEST_F(StringOpsTest, CompareAddressOverrideUsesIndexAndCountWidth) {
    for (bool wide : {false, true}) for (bool cmps : {false, true}) {
        if (wide) m_cpu->init_long_mode(); else m_cpu->init_protected_mode();
        m_cpu->rip = 0x1000;
        const std::vector<u8> code{0x67, 0xF3, static_cast<u8>(cmps ? 0xA6 : 0xAE)};
        write_code(code.data(), code.size());
        const u8 equal[] = {0x55, 0x55};
        write_data(0x2000, equal, sizeof(equal));
        write_data(0x3000, equal, sizeof(equal));
        const u64 high = wide ? 0x1234567800000000ull : 0x12340000ull;
        m_cpu->gpr.set64(RSI, high | 0x2000);
        m_cpu->gpr.set64(RDI, high | 0x3000);
        m_cpu->gpr.set64(RCX, high | 2);
        m_cpu->gpr.set64(RAX, 0x55);
        EXPECT_EQ(execute_one(), ExecResult::Continue);
        const u64 expected_high = wide ? 0 : high;
        EXPECT_EQ(m_cpu->gpr.get64(RDI), expected_high | 0x3002);
        EXPECT_EQ(m_cpu->gpr.get64(RCX), expected_high);
        EXPECT_EQ(m_cpu->gpr.get64(RSI), cmps ? expected_high | 0x2002 : high | 0x2000);
        EXPECT_TRUE(m_cpu->flags.zf());
    }
}

TEST_F(StringOpsTest, CompareUsesImplicitSegmentsAndScasIgnoresOverride) {
    const u8 equal = 0x55, unequal = 0x44;
    for (bool wide : {false, true}) for (bool cmps : {false, true}) {
        if (wide) m_cpu->init_long_mode(); else m_cpu->init_protected_mode();
        m_cpu->rip = 0x1000;
        m_cpu->seg.fs_desc.base = 0x4000;
        if (!wide) {
            m_cpu->seg.ds_desc.base = 0x4000;
            m_cpu->seg.es_desc.base = 0x5000;
        }
        // FS overrides CMPS's source; SCAS still reads ES:(E/R)DI.
        const std::vector<u8> code{0x64, 0xF3, static_cast<u8>(cmps ? 0xA6 : 0xAE)};
        write_code(code.data(), code.size());
        write_data(0x2000, &unequal, 1);
        write_data(0x6000, &equal, 1);
        write_data(wide ? 0x3000 : 0x8000, &equal, 1);
        write_data(0x7000, &unequal, 1);
        m_cpu->gpr.set64(RSI, 0x2000);
        m_cpu->gpr.set64(RDI, 0x3000);
        m_cpu->gpr.set64(RCX, 1);
        m_cpu->gpr.set64(RAX, equal);
        EXPECT_EQ(execute_one(), ExecResult::Continue);
        EXPECT_TRUE(m_cpu->flags.zf());
        EXPECT_EQ(m_cpu->gpr.get64(RDI), 0x3001u);
        EXPECT_EQ(m_cpu->gpr.get64(RSI), cmps ? 0x2001u : 0x2000u);
    }
}

TEST_F(StringOpsTest, CompareSixteenBitIndicesWrapBeforeSegmentAddition) {
    ASSERT_TRUE(m_memory->map_region(0x11000, 0x3000, MemoryProtection::ReadWrite));
    m_cpu->init_protected_mode();
    m_cpu->rip = 0x1000;
    m_cpu->seg.ds_desc.base = 0x2000;
    m_cpu->seg.es_desc.base = 0x3000;
    const u8 code[] = {0x67, 0xF3, 0xA6};
    write_code(code, sizeof(code));
    const u8 value = 0x55;
    for (u64 addr : {0x11FFFull, 0x12FFFull, 0x2000ull, 0x3000ull})
        write_data(addr, &value, 1);
    m_cpu->gpr.set64(RSI, 0x5678FFFF);
    m_cpu->gpr.set64(RDI, 0x9ABCFFFF);
    m_cpu->gpr.set64(RCX, 0x12340002);
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(RSI), 0x56780001u);
    EXPECT_EQ(m_cpu->gpr.get64(RDI), 0x9ABC0001u);
    EXPECT_EQ(m_cpu->gpr.get64(RCX), 0x12340000u);
    EXPECT_TRUE(m_cpu->flags.zf());
}

TEST_F(StringOpsTest, CompareBulkScanDoesNotCrossLinearAddressWrap) {
    ASSERT_TRUE(m_memory->map_region(0, 0x1000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->map_region(0xFFFFF000, 0x2000, MemoryProtection::ReadWrite));
    m_cpu->init_protected_mode();
    m_cpu->rip = 0x1000;
    m_cpu->seg.es_desc.base = 0xFFFFD000;
    const u8 code[] = {0xF3, 0xAE};
    write_code(code, sizeof(code));
    const u8 equal = 0x55, unequal = 0x44;
    write_data(0xFFFFFFFF, &equal, 1);
    write_data(0x100000000ull, &equal, 1); // Decoy, outside linear32 space.
    write_data(0, &unequal, 1);
    m_cpu->gpr.set64(RDI, 0x2FFF);
    m_cpu->gpr.set64(RCX, 2);
    m_cpu->gpr.set64(RAX, equal);
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(RDI), 0x3001u);
    EXPECT_EQ(m_cpu->gpr.get64(RCX), 0u);
    EXPECT_FALSE(m_cpu->flags.zf());
}

// ----------------------------------------------------------------------------
// 1. MOVSB - copies one byte from [RSI] to [RDI]
// ----------------------------------------------------------------------------

TEST_F(StringOpsTest, MovsbSingleByte) {
    // Write source data
    u8 src_data[] = {0xAB};
    write_data(0x2000, src_data, sizeof(src_data));

    m_cpu->gpr.set64(RSI, 0x2000);
    m_cpu->gpr.set64(RDI, 0x3000);

    // MOVSB (A4)
    u8 code[] = {0xA4};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);

    // Check byte was copied
    auto val = m_memory->read_u8(0x3000);
    ASSERT_TRUE(val);
    EXPECT_EQ(val.value(), 0xAB);

    // RSI and RDI should advance by 1 (DF=0)
    EXPECT_EQ(m_cpu->gpr.get64(RSI), 0x2001u);
    EXPECT_EQ(m_cpu->gpr.get64(RDI), 0x3001u);
}

// ----------------------------------------------------------------------------
// 2. REP MOVSB - copies RCX bytes
// ----------------------------------------------------------------------------

TEST_F(StringOpsTest, RepMovsbCopiesRcxBytes) {
    u8 src_data[] = {0x11, 0x22, 0x33, 0x44, 0x55};
    write_data(0x2000, src_data, sizeof(src_data));

    m_cpu->gpr.set64(RSI, 0x2000);
    m_cpu->gpr.set64(RDI, 0x3000);
    m_cpu->gpr.set64(RCX, 5);

    // REP MOVSB (F3 A4)
    u8 code[] = {0xF3, 0xA4};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);

    // All 5 bytes should be copied
    for (usize i = 0; i < 5; ++i) {
        auto val = m_memory->read_u8(0x3000 + i);
        ASSERT_TRUE(val);
        EXPECT_EQ(val.value(), src_data[i]);
    }

    EXPECT_EQ(m_cpu->gpr.get64(RSI), 0x2005u);
    EXPECT_EQ(m_cpu->gpr.get64(RDI), 0x3005u);
    EXPECT_EQ(m_cpu->gpr.get64(RCX), 0u);
}

// ----------------------------------------------------------------------------
// 3. STOSB - stores AL at [RDI]
// ----------------------------------------------------------------------------

TEST_F(StringOpsTest, StosbStoresAlAtRdi) {
    m_cpu->gpr.set8l(RAX, 0x42);
    m_cpu->gpr.set64(RDI, 0x3000);

    // STOSB (AA)
    u8 code[] = {0xAA};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);

    auto val = m_memory->read_u8(0x3000);
    ASSERT_TRUE(val);
    EXPECT_EQ(val.value(), 0x42);

    EXPECT_EQ(m_cpu->gpr.get64(RDI), 0x3001u);
}

// ----------------------------------------------------------------------------
// 4. REP STOSB - fills RCX bytes with AL (memset pattern)
// ----------------------------------------------------------------------------

TEST_F(StringOpsTest, RepStosbFillsMemory) {
    m_cpu->gpr.set8l(RAX, 0xFF);
    m_cpu->gpr.set64(RDI, 0x3000);
    m_cpu->gpr.set64(RCX, 8);

    // REP STOSB (F3 AA)
    u8 code[] = {0xF3, 0xAA};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);

    for (usize i = 0; i < 8; ++i) {
        auto val = m_memory->read_u8(0x3000 + i);
        ASSERT_TRUE(val);
        EXPECT_EQ(val.value(), 0xFF);
    }

    EXPECT_EQ(m_cpu->gpr.get64(RDI), 0x3008u);
    EXPECT_EQ(m_cpu->gpr.get64(RCX), 0u);
}

// ----------------------------------------------------------------------------
// 5. LODSB - loads [RSI] into AL
// ----------------------------------------------------------------------------

TEST_F(StringOpsTest, LodsbLoadsIntoAl) {
    u8 src_data[] = {0xBE};
    write_data(0x2000, src_data, sizeof(src_data));

    m_cpu->gpr.set64(RAX, 0);
    m_cpu->gpr.set64(RSI, 0x2000);

    // LODSB (AC)
    u8 code[] = {0xAC};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);

    EXPECT_EQ(m_cpu->gpr.get8l(RAX), 0xBE);
    EXPECT_EQ(m_cpu->gpr.get64(RSI), 0x2001u);
}

// ----------------------------------------------------------------------------
// 6. CMPSB - compares and sets flags
// ----------------------------------------------------------------------------

TEST_F(StringOpsTest, CmpsbSetsFlags) {
    // Equal bytes
    u8 src[] = {0x42};
    u8 dst[] = {0x42};
    write_data(0x2000, src, sizeof(src));
    write_data(0x3000, dst, sizeof(dst));

    m_cpu->gpr.set64(RSI, 0x2000);
    m_cpu->gpr.set64(RDI, 0x3000);

    // CMPSB (A6)
    u8 code[] = {0xA6};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);

    // Equal: ZF=1, CF=0
    EXPECT_TRUE(m_cpu->flags.zf());
    EXPECT_FALSE(m_cpu->flags.cf());

    EXPECT_EQ(m_cpu->gpr.get64(RSI), 0x2001u);
    EXPECT_EQ(m_cpu->gpr.get64(RDI), 0x3001u);
}

TEST_F(StringOpsTest, CmpsbNotEqual) {
    u8 src[] = {0x10};
    u8 dst[] = {0x20};
    write_data(0x2000, src, sizeof(src));
    write_data(0x3000, dst, sizeof(dst));

    m_cpu->gpr.set64(RSI, 0x2000);
    m_cpu->gpr.set64(RDI, 0x3000);

    u8 code[] = {0xA6};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);

    // Not equal: ZF=0
    EXPECT_FALSE(m_cpu->flags.zf());
    // 0x10 < 0x20: CF=1 (borrow)
    EXPECT_TRUE(m_cpu->flags.cf());
}

// ----------------------------------------------------------------------------
// 7. REPE CMPSB - stops at first mismatch
// ----------------------------------------------------------------------------

TEST_F(StringOpsTest, RepeCmpsbStopsAtMismatch) {
    //               match match mismatch ...
    u8 src[] = {0x41, 0x42, 0x43, 0x44};
    u8 dst[] = {0x41, 0x42, 0x99, 0x44};
    write_data(0x2000, src, sizeof(src));
    write_data(0x3000, dst, sizeof(dst));

    m_cpu->gpr.set64(RSI, 0x2000);
    m_cpu->gpr.set64(RDI, 0x3000);
    m_cpu->gpr.set64(RCX, 4);

    // REPE CMPSB (F3 A6)
    u8 code[] = {0xF3, 0xA6};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);

    // Should have compared 3 elements (2 match, 1 mismatch) then stopped
    EXPECT_EQ(m_cpu->gpr.get64(RCX), 1u);  // 4 - 3 = 1
    EXPECT_FALSE(m_cpu->flags.zf());        // Last compare was mismatch
    EXPECT_EQ(m_cpu->gpr.get64(RSI), 0x2003u);
    EXPECT_EQ(m_cpu->gpr.get64(RDI), 0x3003u);
}

// ----------------------------------------------------------------------------
// 8. REPNE SCASB - finds byte in buffer (strlen-like)
// ----------------------------------------------------------------------------

TEST_F(StringOpsTest, RepneScasbFindsByte) {
    // Buffer: "Hello\0"
    u8 data[] = {'H', 'e', 'l', 'l', 'o', 0x00, 'X'};
    write_data(0x3000, data, sizeof(data));

    m_cpu->gpr.set8l(RAX, 0x00);  // Looking for null terminator
    m_cpu->gpr.set64(RDI, 0x3000);
    m_cpu->gpr.set64(RCX, 7);

    // REPNE SCASB (F2 AE)
    u8 code[] = {0xF2, 0xAE};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);

    // Should have scanned 6 elements (5 != 0, then found 0)
    EXPECT_EQ(m_cpu->gpr.get64(RCX), 1u);  // 7 - 6 = 1
    EXPECT_TRUE(m_cpu->flags.zf());         // Last compare found match (AL == [RDI])
    EXPECT_EQ(m_cpu->gpr.get64(RDI), 0x3006u);  // Past the null byte
}

// ----------------------------------------------------------------------------
// 9. Direction flag: STD then MOVSB copies backward
// ----------------------------------------------------------------------------

TEST_F(StringOpsTest, StdMovsbBackward) {
    u8 src_data[] = {0xAA, 0xBB, 0xCC};
    write_data(0x2000, src_data, sizeof(src_data));

    // Point RSI/RDI at the last byte of the source/dest
    m_cpu->gpr.set64(RSI, 0x2002);
    m_cpu->gpr.set64(RDI, 0x3002);

    // STD (FD) - set direction flag
    u8 code_std[] = {0xFD};
    write_code(code_std, sizeof(code_std));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_TRUE(m_cpu->flags.df());

    // Now execute MOVSB
    m_cpu->rip = 0x1000;
    u8 code_movsb[] = {0xA4};
    write_code(code_movsb, sizeof(code_movsb));
    EXPECT_EQ(execute_one(), ExecResult::Continue);

    // Should have copied 0xCC from 0x2002 to 0x3002
    auto val = m_memory->read_u8(0x3002);
    ASSERT_TRUE(val);
    EXPECT_EQ(val.value(), 0xCC);

    // RSI and RDI should have decremented
    EXPECT_EQ(m_cpu->gpr.get64(RSI), 0x2001u);
    EXPECT_EQ(m_cpu->gpr.get64(RDI), 0x3001u);
}

// ----------------------------------------------------------------------------
// 10. CLD clears direction flag
// ----------------------------------------------------------------------------

TEST_F(StringOpsTest, CldClearsDirectionFlag) {
    // First set DF
    m_cpu->flags.set_df(true);
    EXPECT_TRUE(m_cpu->flags.df());

    // CLD (FC)
    u8 code[] = {0xFC};
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);

    EXPECT_FALSE(m_cpu->flags.df());
}

// ----------------------------------------------------------------------------
// 11. MOVSD - copies 4 bytes
// ----------------------------------------------------------------------------

TEST_F(StringOpsTest, MovsdCopies4Bytes) {
    u8 src_data[] = {0x11, 0x22, 0x33, 0x44};
    write_data(0x2000, src_data, sizeof(src_data));

    m_cpu->gpr.set64(RSI, 0x2000);
    m_cpu->gpr.set64(RDI, 0x3000);

    // MOVSD (A5 in 64-bit mode without REX.W = 32-bit operand)
    u8 code[] = {0xA5};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);

    auto val = m_memory->read_u32(0x3000);
    ASSERT_TRUE(val);
    EXPECT_EQ(val.value(), 0x44332211u);

    EXPECT_EQ(m_cpu->gpr.get64(RSI), 0x2004u);
    EXPECT_EQ(m_cpu->gpr.get64(RDI), 0x3004u);
}

// ----------------------------------------------------------------------------
// 12. REP MOVSQ - copies 8-byte blocks
// ----------------------------------------------------------------------------

TEST_F(StringOpsTest, RepMovsqCopies8ByteBlocks) {
    // Write two 8-byte values
    u8 src_data[] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,  // qword 1
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18   // qword 2
    };
    write_data(0x2000, src_data, sizeof(src_data));

    m_cpu->gpr.set64(RSI, 0x2000);
    m_cpu->gpr.set64(RDI, 0x3000);
    m_cpu->gpr.set64(RCX, 2);

    // REP MOVSQ (F3 48 A5)
    u8 code[] = {0xF3, 0x48, 0xA5};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);

    auto q1 = m_memory->read_u64(0x3000);
    auto q2 = m_memory->read_u64(0x3008);
    ASSERT_TRUE(q1);
    ASSERT_TRUE(q2);
    EXPECT_EQ(q1.value(), 0x0807060504030201ULL);
    EXPECT_EQ(q2.value(), 0x1817161514131211ULL);

    EXPECT_EQ(m_cpu->gpr.get64(RSI), 0x2010u);
    EXPECT_EQ(m_cpu->gpr.get64(RDI), 0x3010u);
    EXPECT_EQ(m_cpu->gpr.get64(RCX), 0u);
}

// ----------------------------------------------------------------------------
// 13. Zero RCX with REP → no operation
// ----------------------------------------------------------------------------

TEST_F(StringOpsTest, RepWithZeroRcxNoOp) {
    m_cpu->gpr.set64(RSI, 0x2000);
    m_cpu->gpr.set64(RDI, 0x3000);
    m_cpu->gpr.set64(RCX, 0);  // Zero count

    // REP MOVSB (F3 A4) - should do nothing
    u8 code[] = {0xF3, 0xA4};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);

    // Registers should not change
    EXPECT_EQ(m_cpu->gpr.get64(RSI), 0x2000u);
    EXPECT_EQ(m_cpu->gpr.get64(RDI), 0x3000u);
    EXPECT_EQ(m_cpu->gpr.get64(RCX), 0u);
}

// ----------------------------------------------------------------------------
// 14. CLC/STC/CMC flag tests
// ----------------------------------------------------------------------------

TEST_F(StringOpsTest, ClcClearsCarryFlag) {
    m_cpu->flags.set_cf(true);
    EXPECT_TRUE(m_cpu->flags.cf());

    // CLC (F8)
    u8 code[] = {0xF8};
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);

    EXPECT_FALSE(m_cpu->flags.cf());
}

TEST_F(StringOpsTest, StcSetsCarryFlag) {
    m_cpu->flags.set_cf(false);
    EXPECT_FALSE(m_cpu->flags.cf());

    // STC (F9)
    u8 code[] = {0xF9};
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);

    EXPECT_TRUE(m_cpu->flags.cf());
}

TEST_F(StringOpsTest, CmcComplementsCarryFlag) {
    // CF=0 -> CF=1
    m_cpu->flags.set_cf(false);

    u8 code[] = {0xF5};
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_TRUE(m_cpu->flags.cf());

    // CF=1 -> CF=0
    m_cpu->rip = 0x1000;
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_FALSE(m_cpu->flags.cf());
}
