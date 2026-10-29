// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// x87 FPU instruction tests (CPU-030)

#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include <gtest/gtest.h>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

using namespace vitreo;
using namespace vitreo::cpu;

// ============================================================================
// Decoder helper
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
// Test Fixture
// ============================================================================

class FpuTest : public ::testing::Test {
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

    ExecResult execute_bytes(std::initializer_list<u8> bytes) {
        u64 addr = m_cpu->rip;
        for (u8 b : bytes) {
            (void)m_memory->write_u8(addr++, b);
        }
        Interpreter interp(*m_cpu, *m_memory);
        return interp.step();
    }

    void write_f32(u64 addr, float val) {
        u32 bits;
        std::memcpy(&bits, &val, 4);
        (void)m_memory->write_u32(addr, bits);
    }

    void write_f64(u64 addr, double val) {
        u64 bits;
        std::memcpy(&bits, &val, 8);
        (void)m_memory->write_u64(addr, bits);
    }

    float read_f32(u64 addr) {
        auto v = m_memory->read_u32(addr);
        u32 bits = v.value();
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }

    double read_f64(u64 addr) {
        auto v = m_memory->read_u64(addr);
        u64 bits = v.value();
        double d;
        std::memcpy(&d, &bits, 8);
        return d;
    }

    void write_i32(u64 addr, i32 val) {
        (void)m_memory->write_u32(addr, static_cast<u32>(val));
    }

    i32 read_i32(u64 addr) {
        auto v = m_memory->read_u32(addr);
        return static_cast<i32>(v.value());
    }

    void write_i64(u64 addr, i64 val) {
        (void)m_memory->write_u64(addr, static_cast<u64>(val));
    }

    i64 read_i64(u64 addr) {
        auto v = m_memory->read_u64(addr);
        return static_cast<i64>(v.value());
    }

    std::unique_ptr<GuestMemory> m_memory;
    std::unique_ptr<CpuState> m_cpu;
};

// ============================================================================
// Decoder Tests
// ============================================================================

TEST(FpuDecoder, Fninit) {
    auto inst = decode({0xDB, 0xE3});
    EXPECT_EQ(inst.opcode, Opcode::FNINIT);
}

TEST(FpuDecoder, Fnop) {
    auto inst = decode({0xD9, 0xD0});
    EXPECT_EQ(inst.opcode, Opcode::FNOP);
}

TEST(FpuDecoder, FldSti) {
    // D9 C0+i — FLD ST(0)
    auto inst = decode({0xD9, 0xC0});
    EXPECT_EQ(inst.opcode, Opcode::FLD_STI);
    EXPECT_EQ(inst.operands[0].imm, 0);

    // D9 C3 — FLD ST(3)
    auto inst3 = decode({0xD9, 0xC3});
    EXPECT_EQ(inst3.opcode, Opcode::FLD_STI);
    EXPECT_EQ(inst3.operands[0].imm, 3);
}

TEST(FpuDecoder, FaddSti) {
    // D8 C1 — FADD ST(0),ST(1)
    auto inst = decode({0xD8, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::FADD_STI);
    EXPECT_EQ(inst.operands[0].imm, 1);
}

TEST(FpuDecoder, FaddpSti) {
    // DE C1 — FADDP ST(1),ST(0)
    auto inst = decode({0xDE, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::FADDP_STI);
    EXPECT_EQ(inst.operands[0].imm, 1);
}

TEST(FpuDecoder, Fcompp) {
    auto inst = decode({0xDE, 0xD9});
    EXPECT_EQ(inst.opcode, Opcode::FCOMPP);
}

TEST(FpuDecoder, FnstswAx) {
    auto inst = decode({0xDF, 0xE0});
    EXPECT_EQ(inst.opcode, Opcode::FNSTSW_AX);
}

TEST(FpuDecoder, Fchs) {
    auto inst = decode({0xD9, 0xE0});
    EXPECT_EQ(inst.opcode, Opcode::FCHS);
}

TEST(FpuDecoder, Fabs) {
    auto inst = decode({0xD9, 0xE1});
    EXPECT_EQ(inst.opcode, Opcode::FABS);
}

TEST(FpuDecoder, Fld1) {
    auto inst = decode({0xD9, 0xE8});
    EXPECT_EQ(inst.opcode, Opcode::FLD1);
}

TEST(FpuDecoder, Fldz) {
    auto inst = decode({0xD9, 0xEE});
    EXPECT_EQ(inst.opcode, Opcode::FLDZ);
}

TEST(FpuDecoder, FxchSti) {
    // D9 C9 — FXCH ST(1)
    auto inst = decode({0xD9, 0xC9});
    EXPECT_EQ(inst.opcode, Opcode::FXCH_STI);
    EXPECT_EQ(inst.operands[0].imm, 1);
}

TEST(FpuDecoder, Fdecstp) {
    auto inst = decode({0xD9, 0xF6});
    EXPECT_EQ(inst.opcode, Opcode::FDECSTP);
}

TEST(FpuDecoder, Fincstp) {
    auto inst = decode({0xD9, 0xF7});
    EXPECT_EQ(inst.opcode, Opcode::FINCSTP);
}

TEST(FpuDecoder, Ftst) {
    auto inst = decode({0xD9, 0xE4});
    EXPECT_EQ(inst.opcode, Opcode::FTST);
}

TEST(FpuDecoder, FidivM32) {
    auto inst = decode({0xDA, 0x70, 0x08}, DecodeMode::Mode32);
    EXPECT_EQ(inst.opcode, Opcode::FIDIV_M32);
    ASSERT_EQ(inst.operand_count, 1);
    EXPECT_EQ(inst.operands[0].type, OperandType::Mem32);
    EXPECT_EQ(inst.length, 3);
}

TEST(FpuDecoder, FimulM32) {
    // DA 4D FC — FIMUL m32int [EBP-4] (Starcraft/storm.dll illegal-insn site)
    auto inst = decode({0xDA, 0x4D, 0xFC}, DecodeMode::Mode32);
    EXPECT_EQ(inst.opcode, Opcode::FIMUL_M32);
    ASSERT_EQ(inst.operand_count, 1);
    EXPECT_EQ(inst.operands[0].type, OperandType::Mem32);
    EXPECT_EQ(inst.length, 3);
}

TEST(FpuDecoder, FiaddM16) {
    // DE 00 — FIADD m16int [EAX]
    auto inst = decode({0xDE, 0x00}, DecodeMode::Mode32);
    EXPECT_EQ(inst.opcode, Opcode::FIADD_M16);
    ASSERT_EQ(inst.operand_count, 1);
    EXPECT_EQ(inst.operands[0].type, OperandType::Mem16);
}

// ============================================================================
// Execution Tests
// ============================================================================

// 1. FNINIT
TEST_F(FpuTest, Fninit) {
    // Dirty up state first
    m_cpu->fpu.control_word = 0x1234;
    m_cpu->fpu.status_word = 0x5678;
    m_cpu->fpu.tag_word = 0x0000;

    auto result = execute_bytes({0xDB, 0xE3});
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.control_word, 0x037F);
    EXPECT_EQ(m_cpu->fpu.status_word, 0);
    EXPECT_EQ(m_cpu->fpu.tag_word, 0xFFFF);
}

TEST_F(FpuTest, FidivM32UsesSignedIntegerWithoutPopping) {
    constexpr u64 data_addr = 0x2000;
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RAX), data_addr - 8);
    write_i32(data_addr, -7);
    m_cpu->fpu.push(Float80(84.0));
    const u8 top_before = m_cpu->fpu.top();

    const auto result = execute_bytes({0xDA, 0x70, 0x08});

    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_DOUBLE_EQ(m_cpu->fpu.get_st(0).to_double(), -12.0);
    EXPECT_EQ(m_cpu->fpu.top(), top_before);
    EXPECT_EQ(read_i32(data_addr), -7);
}

TEST_F(FpuTest, FimulM32UsesSignedIntegerWithoutPopping) {
    constexpr u64 data_addr = 0x2000;
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RAX), data_addr - 8);
    write_i32(data_addr, -3);
    m_cpu->fpu.push(Float80(5.0));
    const u8 top_before = m_cpu->fpu.top();

    // DA 48 08 — FIMUL dword [RAX+8]
    const auto result = execute_bytes({0xDA, 0x48, 0x08});

    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_DOUBLE_EQ(m_cpu->fpu.get_st(0).to_double(), -15.0);
    EXPECT_EQ(m_cpu->fpu.top(), top_before);
}

TEST_F(FpuTest, FiaddM16UsesSignedInteger) {
    constexpr u64 data_addr = 0x2000;
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RAX), data_addr);
    (void)m_memory->write_u16(data_addr, static_cast<u16>(static_cast<i16>(-4)));
    m_cpu->fpu.push(Float80(10.0));

    // DE 00 — FIADD word [RAX]
    const auto result = execute_bytes({0xDE, 0x00});

    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_DOUBLE_EQ(m_cpu->fpu.get_st(0).to_double(), 6.0);
}

TEST_F(FpuTest, FicompM32ComparesAndPops) {
    constexpr u64 data_addr = 0x2000;
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RAX), data_addr);
    write_i32(data_addr, 7);
    m_cpu->fpu.push(Float80(1.0)); // will become ST(1)
    m_cpu->fpu.push(Float80(3.0)); // ST(0)
    const u8 top_before = m_cpu->fpu.top();

    // DA 18 — FICOMP dword [RAX]
    const auto result = execute_bytes({0xDA, 0x18});

    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.top(), static_cast<u8>((top_before + 1) & 7));
    // 3 < 7 → C0=1, C2=0, C3=0
    EXPECT_TRUE(m_cpu->fpu.status_word & (1u << 8));   // C0
    EXPECT_FALSE(m_cpu->fpu.status_word & (1u << 10)); // C2
    EXPECT_FALSE(m_cpu->fpu.status_word & (1u << 14)); // C3
}

// 2. FLD m32fp + FSTP m32fp round-trip
TEST_F(FpuTest, FldM32RoundTrip) {
    const float test_val = 3.14f;
    const u64 data_addr = 0x2000;

    // Set RBX = data_addr
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), data_addr);
    write_f32(data_addr, test_val);

    // FLD m32 [RBX]: D9 03
    auto r1 = execute_bytes({0xD9, 0x03});
    EXPECT_EQ(r1, ExecResult::Continue);
    EXPECT_NEAR(m_cpu->fpu.get_st(0).to_double(), static_cast<double>(test_val), 1e-5);

    // FSTP m32 [RBX] to address 0x2010: D9 1B
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), data_addr + 0x10);
    auto r2 = execute_bytes({0xD9, 0x1B});
    EXPECT_EQ(r2, ExecResult::Continue);
    EXPECT_FLOAT_EQ(read_f32(data_addr + 0x10), test_val);
}

// 3. FLD m64fp + FSTP m64fp round-trip
TEST_F(FpuTest, FldM64RoundTrip) {
    const double test_val = 2.718281828459045;
    const u64 data_addr = 0x2000;

    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), data_addr);
    write_f64(data_addr, test_val);

    // FLD m64 [RBX]: DD 03
    auto r1 = execute_bytes({0xDD, 0x03});
    EXPECT_EQ(r1, ExecResult::Continue);
    EXPECT_DOUBLE_EQ(m_cpu->fpu.get_st(0).to_double(), test_val);

    // FSTP m64 [RBX] to address 0x2010: DD 1B
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), data_addr + 0x10);
    auto r2 = execute_bytes({0xDD, 0x1B});
    EXPECT_EQ(r2, ExecResult::Continue);
    EXPECT_DOUBLE_EQ(read_f64(data_addr + 0x10), test_val);
}

TEST_F(FpuTest, FldM64PreservesSmallestSubnormal) {
    const u64 data_addr = 0x2000;
    constexpr u64 smallest_binary64_subnormal = 1;

    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), data_addr);
    ASSERT_TRUE(m_memory->write_u64(data_addr, smallest_binary64_subnormal));

    ASSERT_EQ(execute_bytes({0xDD, 0x03}), ExecResult::Continue); // FLD m64 [RBX]

    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), data_addr + 0x10);
    ASSERT_EQ(execute_bytes({0xDD, 0x1B}), ExecResult::Continue); // FSTP m64 [RBX]

    auto stored = m_memory->read_u64(data_addr + 0x10);
    ASSERT_TRUE(stored);
    EXPECT_EQ(stored.value(), smallest_binary64_subnormal);
}

// 4. FILD m32 + FISTP m32 round-trip
TEST_F(FpuTest, FildFistpM32RoundTrip) {
    const i32 test_val = 42;
    const u64 data_addr = 0x2000;

    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), data_addr);
    write_i32(data_addr, test_val);

    // FILD m32 [RBX]: DB 03
    auto r1 = execute_bytes({0xDB, 0x03});
    EXPECT_EQ(r1, ExecResult::Continue);
    EXPECT_DOUBLE_EQ(m_cpu->fpu.get_st(0).to_double(), 42.0);

    // FISTP m32 [RBX] output: DB 1B
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), data_addr + 0x10);
    auto r2 = execute_bytes({0xDB, 0x1B});
    EXPECT_EQ(r2, ExecResult::Continue);
    EXPECT_EQ(read_i32(data_addr + 0x10), test_val);
}

// 5. FADD: load two values, FADDP, check result
TEST_F(FpuTest, Faddp) {
    const u64 data_addr = 0x2000;
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), data_addr);

    // Load 3.0 then 4.0
    write_f64(data_addr, 3.0);
    execute_bytes({0xDD, 0x03}); // FLD m64 [RBX]

    write_f64(data_addr + 8, 4.0);
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), data_addr + 8);
    execute_bytes({0xDD, 0x03}); // FLD m64 [RBX]

    // FADDP ST(1),ST(0): DE C1
    auto r = execute_bytes({0xDE, 0xC1});
    EXPECT_EQ(r, ExecResult::Continue);
    EXPECT_DOUBLE_EQ(m_cpu->fpu.get_st(0).to_double(), 7.0);
}

// 6. FSUB: load 10.0 and 3.0, FSUBP, check 7.0
TEST_F(FpuTest, Fsubp) {
    const u64 data_addr = 0x2000;
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), data_addr);

    // Load 10.0 first (will be ST(1) after second load)
    write_f64(data_addr, 10.0);
    execute_bytes({0xDD, 0x03});

    // Load 3.0 (becomes ST(0))
    write_f64(data_addr + 8, 3.0);
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), data_addr + 8);
    execute_bytes({0xDD, 0x03});

    // FSUBP ST(1),ST(0): DE E9
    // Result = ST(1) - ST(0) = 10.0 - 3.0 = 7.0
    auto r = execute_bytes({0xDE, 0xE9});
    EXPECT_EQ(r, ExecResult::Continue);
    EXPECT_DOUBLE_EQ(m_cpu->fpu.get_st(0).to_double(), 7.0);
}

// 7. FMUL: 3.0 * 4.0 = 12.0
TEST_F(FpuTest, Fmulp) {
    const u64 data_addr = 0x2000;
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), data_addr);

    write_f64(data_addr, 3.0);
    execute_bytes({0xDD, 0x03});

    write_f64(data_addr + 8, 4.0);
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), data_addr + 8);
    execute_bytes({0xDD, 0x03});

    // FMULP ST(1),ST(0): DE C9
    auto r = execute_bytes({0xDE, 0xC9});
    EXPECT_EQ(r, ExecResult::Continue);
    EXPECT_DOUBLE_EQ(m_cpu->fpu.get_st(0).to_double(), 12.0);
}

// 8. FDIV: 12.0 / 4.0 = 3.0
TEST_F(FpuTest, Fdivp) {
    const u64 data_addr = 0x2000;
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), data_addr);

    // Load 12.0 (will be ST(1))
    write_f64(data_addr, 12.0);
    execute_bytes({0xDD, 0x03});

    // Load 4.0 (ST(0))
    write_f64(data_addr + 8, 4.0);
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), data_addr + 8);
    execute_bytes({0xDD, 0x03});

    // FDIVP ST(1),ST(0): DE F9
    // Result = ST(1) / ST(0) = 12.0 / 4.0 = 3.0
    auto r = execute_bytes({0xDE, 0xF9});
    EXPECT_EQ(r, ExecResult::Continue);
    EXPECT_DOUBLE_EQ(m_cpu->fpu.get_st(0).to_double(), 3.0);
}

// 9. FCOM sets status flags
TEST_F(FpuTest, FcomStatusFlags) {
    // Push 5.0, then 3.0; ST(0)=3.0, ST(1)=5.0
    m_cpu->fpu.push(Float80(5.0));
    m_cpu->fpu.push(Float80(3.0));

    // FCOM ST(1): D8 D1  — compare ST(0)=3.0 with ST(1)=5.0 → ST(0) < ST(1)
    auto r = execute_bytes({0xD8, 0xD1});
    EXPECT_EQ(r, ExecResult::Continue);
    // C0=1 (less than)
    EXPECT_NE(m_cpu->fpu.status_word & FPU_SW_C0, 0);
    EXPECT_EQ(m_cpu->fpu.status_word & FPU_SW_C2, 0);
    EXPECT_EQ(m_cpu->fpu.status_word & FPU_SW_C3, 0);

    // Now push 10.0 on top; ST(0)=10.0, ST(1)=3.0
    m_cpu->fpu.push(Float80(10.0));

    // FCOM ST(1): compare ST(0)=10.0 with ST(1)=3.0 → ST(0) > ST(1)
    auto r2 = execute_bytes({0xD8, 0xD1});
    EXPECT_EQ(r2, ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.status_word & FPU_SW_C0, 0);
    EXPECT_EQ(m_cpu->fpu.status_word & FPU_SW_C2, 0);
    EXPECT_EQ(m_cpu->fpu.status_word & FPU_SW_C3, 0);

    // Push equal value
    m_cpu->fpu.push(Float80(10.0));
    auto top_val = m_cpu->fpu.get_st(0).to_double();
    auto st1_val = m_cpu->fpu.get_st(1).to_double();
    EXPECT_DOUBLE_EQ(top_val, st1_val);

    auto r3 = execute_bytes({0xD8, 0xD1});
    EXPECT_EQ(r3, ExecResult::Continue);
    // C3=1 (equal)
    EXPECT_NE(m_cpu->fpu.status_word & FPU_SW_C3, 0);
    EXPECT_EQ(m_cpu->fpu.status_word & FPU_SW_C0, 0);
}

// 10. FXCH swaps registers
TEST_F(FpuTest, Fxch) {
    m_cpu->fpu.push(Float80(1.0));
    m_cpu->fpu.push(Float80(2.0));
    // ST(0)=2.0  ST(1)=1.0

    // FXCH ST(1): D9 C9
    auto r = execute_bytes({0xD9, 0xC9});
    EXPECT_EQ(r, ExecResult::Continue);
    EXPECT_DOUBLE_EQ(m_cpu->fpu.get_st(0).to_double(), 1.0);
    EXPECT_DOUBLE_EQ(m_cpu->fpu.get_st(1).to_double(), 2.0);
}

// 11. FCHS negates ST(0)
TEST_F(FpuTest, Fchs) {
    m_cpu->fpu.push(Float80(42.0));

    // FCHS: D9 E0
    auto r = execute_bytes({0xD9, 0xE0});
    EXPECT_EQ(r, ExecResult::Continue);
    EXPECT_DOUBLE_EQ(m_cpu->fpu.get_st(0).to_double(), -42.0);

    // Double negate
    auto r2 = execute_bytes({0xD9, 0xE0});
    EXPECT_EQ(r2, ExecResult::Continue);
    EXPECT_DOUBLE_EQ(m_cpu->fpu.get_st(0).to_double(), 42.0);
}

// 12. FABS takes absolute value
TEST_F(FpuTest, Fabs) {
    m_cpu->fpu.push(Float80(-99.5));

    // FABS: D9 E1
    auto r = execute_bytes({0xD9, 0xE1});
    EXPECT_EQ(r, ExecResult::Continue);
    EXPECT_DOUBLE_EQ(m_cpu->fpu.get_st(0).to_double(), 99.5);
}

// 13. FLD1 pushes 1.0
TEST_F(FpuTest, Fld1) {
    // FLD1: D9 E8
    auto r = execute_bytes({0xD9, 0xE8});
    EXPECT_EQ(r, ExecResult::Continue);
    EXPECT_DOUBLE_EQ(m_cpu->fpu.get_st(0).to_double(), 1.0);
}

// 14. FLDZ pushes 0.0
TEST_F(FpuTest, Fldz) {
    // FLDZ: D9 EE
    auto r = execute_bytes({0xD9, 0xEE});
    EXPECT_EQ(r, ExecResult::Continue);
    EXPECT_DOUBLE_EQ(m_cpu->fpu.get_st(0).to_double(), 0.0);
    EXPECT_TRUE(m_cpu->fpu.get_st(0).is_zero());
}

TEST_F(FpuTest, ConstantPushStackOverflowMaskedCommitsIndefinite) {
    m_cpu->fpu.control_word = 0x027F; // invalid operation masked
    m_cpu->fpu.status_word = 0;
    m_cpu->fpu.tag_word = 0x0000; // every physical register occupied
    for (u8 i = 0; i < 8; ++i) m_cpu->fpu.st[i] = Float80(double(i + 1));

    EXPECT_EQ(execute_bytes({0xD9, 0xE8}), ExecResult::Continue); // FLD1
    EXPECT_EQ(m_cpu->fpu.status_word, 0x3A41u);
    EXPECT_EQ(m_cpu->fpu.top(), 7u);
    EXPECT_EQ(m_cpu->fpu.tag_word, 0x8000u);
    const Float80 indefinite = FpuState::indefinite();
    EXPECT_EQ(std::memcmp(&m_cpu->fpu.st[7], &indefinite,
                          sizeof(indefinite)), 0);
    for (u8 i = 0; i < 7; ++i) {
        EXPECT_DOUBLE_EQ(m_cpu->fpu.st[i].to_double(), double(i + 1));
    }
}

TEST_F(FpuTest, ConstantPushStackOverflowUnmaskedDefersWithoutCommit) {
    m_cpu->fpu.control_word = 0x027E; // invalid operation unmasked
    m_cpu->fpu.status_word = 0;
    m_cpu->fpu.tag_word = 0x0000;
    for (u8 i = 0; i < 8; ++i) m_cpu->fpu.st[i] = Float80(double(i + 1));

    EXPECT_EQ(execute_bytes({0xD9, 0xEE}), ExecResult::Continue); // FLDZ
    EXPECT_EQ(m_cpu->fpu.status_word, 0x82C1u);
    EXPECT_EQ(m_cpu->fpu.top(), 0u);
    EXPECT_EQ(m_cpu->fpu.tag_word, 0x0000u);
    for (u8 i = 0; i < 8; ++i) {
        EXPECT_DOUBLE_EQ(m_cpu->fpu.st[i].to_double(), double(i + 1));
    }

    EXPECT_EQ(execute_bytes({0x9B}), ExecResult::Exception); // FWAIT
}

TEST_F(FpuTest, EmptyStackArithmeticUnmaskedInvalidDefersWithoutCommit) {
    m_cpu->fpu.control_word = 0x027E; // invalid operation unmasked
    m_cpu->fpu.status_word = 0;
    m_cpu->fpu.tag_word = 0xFFFF;
    const FpuState before = m_cpu->fpu;

    EXPECT_EQ(execute_bytes({0xD8, 0xC0}), ExecResult::Continue); // FADD ST0,ST0
    EXPECT_EQ(m_cpu->fpu.status_word, 0x80C1u);
    EXPECT_EQ(m_cpu->fpu.tag_word, before.tag_word);
    EXPECT_EQ(m_cpu->fpu.top(), before.top());
    EXPECT_EQ(std::memcmp(m_cpu->fpu.st, before.st, sizeof(before.st)), 0);

    m_cpu->rip += 2;
    EXPECT_EQ(execute_bytes({0x9B}), ExecResult::Exception); // FWAIT
}

TEST_F(FpuTest, EmptyStackStoreUnmaskedInvalidDoesNotWriteOrPop) {
    constexpr u64 kDestination = 0x2000;
    constexpr u64 kSentinel = 0x1122334455667788ULL;
    ASSERT_TRUE(m_memory->write_u64(kDestination, kSentinel).is_ok());
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), kDestination);
    m_cpu->fpu.control_word = 0x027E;
    m_cpu->fpu.status_word = 0;
    m_cpu->fpu.tag_word = 0xFFFF;

    EXPECT_EQ(execute_bytes({0xDD, 0x1B}), ExecResult::Continue); // FSTP m64
    ASSERT_TRUE(m_memory->read_u64(kDestination).is_ok());
    EXPECT_EQ(m_memory->read_u64(kDestination).value(), kSentinel);
    EXPECT_EQ(m_cpu->fpu.status_word, 0x80C1u);
    EXPECT_EQ(m_cpu->fpu.tag_word, 0xFFFFu);
    EXPECT_EQ(m_cpu->fpu.top(), 0u);
}

// Conversion exceptions are produced by the store instruction itself, after
// its source has been read but before either architectural destination commits.
TEST_F(FpuTest, IntegerStoresSuppressWriteAndPopForUnmaskedConversionFaults) {
    constexpr u64 kDestination = 0x2000;
    constexpr u32 kSentinel = 0xA5A55A5Au;
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), kDestination);

    // FISTP m32 of 1.5 raises #P and rounds upward under nearest-even. With
    // precision unmasked, neither the rounded integer nor the pop commits.
    m_cpu->fpu.reset();
    m_cpu->fpu.control_word = 0x035F;
    m_cpu->fpu.push(Float80(1.5));
    ASSERT_TRUE(m_memory->write_u32(kDestination, kSentinel).is_ok());
    const u8 precision_top = m_cpu->fpu.top();
    const u16 precision_tags = m_cpu->fpu.tag_word;
    EXPECT_EQ(execute_bytes({0xDB, 0x1B}), ExecResult::Continue); // FISTP m32
    ASSERT_TRUE(m_memory->read_u32(kDestination).is_ok());
    EXPECT_EQ(m_memory->read_u32(kDestination).value(), kSentinel);
    EXPECT_EQ(m_cpu->fpu.top(), precision_top);
    EXPECT_EQ(m_cpu->fpu.tag_word, precision_tags);
    EXPECT_EQ(m_cpu->fpu.status_word,
              static_cast<u16>(0x3800u | FPU_SW_B | FPU_SW_ES |
                               FPU_SW_C1 | FPU_SW_PE));

    // FISTP m16 of +32768 raises #IA. With invalid unmasked, integer
    // indefinite is not written and the source remains live.
    m_cpu->fpu.reset();
    m_cpu->fpu.control_word = 0x037E;
    m_cpu->fpu.push(Float80(32768.0));
    ASSERT_TRUE(m_memory->write_u32(kDestination, kSentinel).is_ok());
    const u8 invalid_top = m_cpu->fpu.top();
    const u16 invalid_tags = m_cpu->fpu.tag_word;
    EXPECT_EQ(execute_bytes({0xDF, 0x1B}), ExecResult::Continue); // FISTP m16
    ASSERT_TRUE(m_memory->read_u32(kDestination).is_ok());
    EXPECT_EQ(m_memory->read_u32(kDestination).value(), kSentinel);
    EXPECT_EQ(m_cpu->fpu.top(), invalid_top);
    EXPECT_EQ(m_cpu->fpu.tag_word, invalid_tags);
    EXPECT_EQ(m_cpu->fpu.status_word,
              static_cast<u16>(0x3800u | FPU_SW_B | FPU_SW_ES | FPU_SW_IE));
}

TEST_F(FpuTest, DivideByZeroUnmaskedDefersWithoutCommit) {
    m_cpu->fpu.control_word = 0x027B; // divide-by-zero unmasked
    m_cpu->fpu.status_word = 0;
    m_cpu->fpu.tag_word = 0xFFF4; // physical ST0 valid, ST1 zero
    m_cpu->fpu.st[0] = Float80(1.0);
    m_cpu->fpu.st[1] = Float80(0.0);
    const FpuState before = m_cpu->fpu;

    EXPECT_EQ(execute_bytes({0xD8, 0xF1}), ExecResult::Continue); // FDIV ST0,ST1
    EXPECT_EQ(m_cpu->fpu.status_word, 0x8084u);
    EXPECT_EQ(m_cpu->fpu.tag_word, before.tag_word);
    EXPECT_EQ(std::memcmp(m_cpu->fpu.st, before.st, sizeof(before.st)), 0);

    m_cpu->rip += 2;
    EXPECT_EQ(execute_bytes({0x9B}), ExecResult::Exception); // FWAIT
}

TEST_F(FpuTest, OrderedNanCompareHonorsInvalidMaskAndCommitRule) {
    const Float80 qnan = FpuState::indefinite();
    m_cpu->fpu.control_word = 0x027E; // invalid operation unmasked
    m_cpu->fpu.status_word = FPU_SW_C3;
    m_cpu->fpu.tag_word = 0xFFFE; // ST0 special, remaining registers empty
    m_cpu->fpu.st[0] = qnan;

    EXPECT_EQ(execute_bytes({0xD8, 0xD0}), ExecResult::Continue); // FCOM ST0
    EXPECT_EQ(m_cpu->fpu.status_word,
              static_cast<u16>(FPU_SW_B | FPU_SW_ES | FPU_SW_IE | FPU_SW_C3));

    m_cpu->fpu.control_word = 0x027F; // mask invalid operation
    m_cpu->fpu.status_word = 0;
    EXPECT_EQ(execute_bytes({0xD8, 0xD0}), ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.status_word,
              static_cast<u16>(FPU_SW_IE | FPU_SW_C0 | FPU_SW_C2 | FPU_SW_C3));
}

TEST_F(FpuTest, MultiResultStackOverflowUnmaskedRollsBackAtomically) {
    constexpr std::array<std::array<u8, 2>, 3> instructions{{
        {{0xD9, 0xFB}}, // FSINCOS
        {{0xD9, 0xF2}}, // FPTAN
        {{0xD9, 0xF4}}, // FXTRACT
    }};
    for (const auto& bytes : instructions) {
        m_cpu->fpu.reset();
        m_cpu->fpu.control_word = 0x027E;
        m_cpu->fpu.tag_word = 0x0000;
        for (u8 i = 0; i < 8; ++i) m_cpu->fpu.st[i] = Float80(double(i + 1));
        const FpuState before = m_cpu->fpu;

        EXPECT_EQ(execute_bytes({bytes[0], bytes[1]}), ExecResult::Continue);
        EXPECT_EQ(m_cpu->fpu.status_word, 0x82C1u);
        EXPECT_EQ(m_cpu->fpu.tag_word, before.tag_word);
        EXPECT_EQ(m_cpu->fpu.top(), before.top());
        EXPECT_EQ(std::memcmp(m_cpu->fpu.st, before.st, sizeof(before.st)), 0);
        m_cpu->rip += 2;
    }
}

TEST_F(FpuTest, MultiResultStackOverflowMaskedCompletesWithTwoIndefinites) {
    constexpr std::array<std::array<u8, 2>, 3> instructions{{
        {{0xD9, 0xFB}}, {{0xD9, 0xF2}}, {{0xD9, 0xF4}},
    }};
    const Float80 indefinite = FpuState::indefinite();
    for (const auto& bytes : instructions) {
        m_cpu->fpu.reset();
        m_cpu->fpu.control_word = 0x027F;
        m_cpu->fpu.tag_word = 0x0000;
        for (u8 i = 0; i < 8; ++i) m_cpu->fpu.st[i] = Float80(double(i + 1));

        EXPECT_EQ(execute_bytes({bytes[0], bytes[1]}), ExecResult::Continue);
        EXPECT_EQ(m_cpu->fpu.status_word, 0x3A41u);
        EXPECT_EQ(m_cpu->fpu.top(), 7u);
        EXPECT_EQ(m_cpu->fpu.tag_word, 0x8002u);
        EXPECT_EQ(std::memcmp(&m_cpu->fpu.get_st(0), &indefinite,
                              sizeof(indefinite)), 0);
        EXPECT_EQ(std::memcmp(&m_cpu->fpu.get_st(1), &indefinite,
                              sizeof(indefinite)), 0);
        for (u8 i = 2; i < 8; ++i) {
            EXPECT_DOUBLE_EQ(m_cpu->fpu.get_st(i).to_double(), double(i));
        }
        m_cpu->rip += 2;
    }
}

TEST_F(FpuTest, HostArithmeticHarvestSetsSummaryForUnmaskedClasses) {
    const auto exercise = [&](u16 control_word, Float80 a, Float80 b,
                              X87Op op, u16 expected) {
        m_cpu->fpu.reset();
        m_cpu->fpu.control_word = control_word;
        m_cpu->fpu.status_word = 0;
        (void)m_cpu->fpu.arith_x87(a, b, op);
        EXPECT_NE(m_cpu->fpu.status_word & expected, 0u);
        EXPECT_NE(m_cpu->fpu.status_word & FPU_SW_ES, 0u);
        EXPECT_NE(m_cpu->fpu.status_word & FPU_SW_B, 0u);
    };
    exercise(0x0277, Float80(std::numeric_limits<double>::max()),
             Float80(2.0), X87Op::Mul, FPU_SW_OE);
    exercise(0x026F, Float80(std::numeric_limits<double>::min()),
             Float80(std::numeric_limits<double>::min()), X87Op::Mul,
             FPU_SW_UE);
    exercise(0x025F, Float80(1.0), Float80(3.0), X87Op::Div, FPU_SW_PE);
}

TEST(Float80SoftTest, UnmaskedRangeExceptionsReturnExponentScaledValues) {
    constexpr math::SoftF80 kLargest{
        0xFFFFFFFFFFFFFFFFull, 0x7FFEu};
    constexpr math::SoftF80 kSmallestNormal{
        0x8000000000000000ull, 0x0001u};
    constexpr math::SoftF80 kTwo{
        0x8000000000000000ull, 0x4000u};
    constexpr math::SoftF80 kThree{
        0xC000000000000000ull, 0x4000u};

    u16 overflow_status = 0;
    const math::SoftF80 overflow =
        math::f80_mul(kLargest, kTwo, 0x0377, &overflow_status);
    EXPECT_EQ(overflow.significand, 0xFFFFFFFFFFFFFFFFull);
    EXPECT_EQ(overflow.sign_exp, 0x1FFFu);
    EXPECT_EQ(overflow_status, math::kF80Overflow);

    u16 underflow_status = 0;
    const math::SoftF80 underflow =
        math::f80_div(kSmallestNormal, kThree, 0x036F,
                      &underflow_status);
    EXPECT_EQ(underflow.significand, 0xAAAAAAAAAAAAAAABull);
    EXPECT_EQ(underflow.sign_exp, 0x5FFFu);
    EXPECT_EQ(underflow_status,
              static_cast<u16>(math::kF80Underflow |
                               math::kF80Precision | math::kF80C1));
}

TEST_F(FpuTest, UnmaskedOverflowCommitsPcRcRoundedScaledRegisterResult) {
    const Float80 largest = Float80::from_extended(
        0xFFFFFFFFFFFFFFFFull, 0x7FFEu);
    const Float80 two(2.0);

    // PC=64, nearest: the significand is exact, so the unmasked completion
    // raises #O without #P and commits exponent 16384-24576.
    m_cpu->fpu.control_word = 0x0377; // overflow unmasked
    m_cpu->fpu.status_word = 0;
    const Float80 pc64 =
        m_cpu->fpu.arith_x87(largest, two, X87Op::Mul);
    EXPECT_EQ(pc64.mantissa, 0xFFFFFFFFFFFFFFFFull);
    EXPECT_EQ(pc64.exponent_sign, 0x1FFFu);
    EXPECT_EQ(m_cpu->fpu.status_word,
              static_cast<u16>(FPU_SW_B | FPU_SW_ES |
                               FPU_SW_OE));

    // PC=24, nearest: rounding the all-ones significand carries into the
    // exponent before the 24576 adjustment, and C1 reports that round-up.
    m_cpu->fpu.control_word = 0x0077;
    m_cpu->fpu.status_word = 0;
    const Float80 pc24 =
        m_cpu->fpu.arith_x87(largest, two, X87Op::Mul);
    EXPECT_EQ(pc24.mantissa, 0x8000000000000000ull);
    EXPECT_EQ(pc24.exponent_sign, 0x2000u);
    EXPECT_EQ(m_cpu->fpu.status_word,
              static_cast<u16>(FPU_SW_B | FPU_SW_ES | FPU_SW_C1 |
                               FPU_SW_OE | FPU_SW_PE));

    // PC=24, round-toward-zero keeps the largest 24-bit significand instead
    // of carrying.  #P is set and C1 is clear because magnitude did not grow.
    m_cpu->fpu.control_word = 0x0C77;
    m_cpu->fpu.status_word = 0;
    const Float80 chopped =
        m_cpu->fpu.arith_x87(largest, two, X87Op::Mul);
    EXPECT_EQ(chopped.mantissa, 0xFFFFFF0000000000ull);
    EXPECT_EQ(chopped.exponent_sign, 0x1FFFu);
    EXPECT_EQ(m_cpu->fpu.status_word,
              static_cast<u16>(FPU_SW_B | FPU_SW_ES |
                               FPU_SW_OE | FPU_SW_PE));

    // The existing masked completion remains infinity under nearest.
    m_cpu->fpu.control_word = 0x037F;
    m_cpu->fpu.status_word = 0;
    const Float80 masked =
        m_cpu->fpu.arith_x87(largest, two, X87Op::Mul);
    EXPECT_EQ(masked.mantissa, 0x8000000000000000ull);
    EXPECT_EQ(masked.exponent_sign, 0x7FFFu);
    EXPECT_EQ(m_cpu->fpu.status_word,
              static_cast<u16>(FPU_SW_C1 | FPU_SW_OE | FPU_SW_PE));
}

TEST_F(FpuTest, UnmaskedUnderflowCommitsPcRcRoundedScaledRegisterResult) {
    const Float80 smallest_normal = Float80::from_extended(
        0x8000000000000000ull, 0x0001u);
    const Float80 three(3.0);

    m_cpu->fpu.control_word = 0x036F; // PC=64, nearest, underflow unmasked
    m_cpu->fpu.status_word = 0;
    const Float80 scaled =
        m_cpu->fpu.arith_x87(smallest_normal, three, X87Op::Div);
    EXPECT_EQ(scaled.mantissa, 0xAAAAAAAAAAAAAAABull);
    EXPECT_EQ(scaled.exponent_sign, 0x5FFFu);
    EXPECT_EQ(m_cpu->fpu.status_word,
              static_cast<u16>(FPU_SW_B | FPU_SW_ES | FPU_SW_C1 |
                               FPU_SW_UE | FPU_SW_PE));

    // With #U masked, gradual underflow remains the architectural completion.
    m_cpu->fpu.control_word = 0x037F;
    m_cpu->fpu.status_word = 0;
    const Float80 masked =
        m_cpu->fpu.arith_x87(smallest_normal, three, X87Op::Div);
    EXPECT_EQ(masked.exponent_sign, 0u);
    EXPECT_NE(masked.mantissa, 0u);
    EXPECT_EQ(m_cpu->fpu.status_word &
                  static_cast<u16>(FPU_SW_UE | FPU_SW_PE),
              static_cast<u16>(FPU_SW_UE | FPU_SW_PE));
}

TEST_F(FpuTest, UnmaskedUnderflowInstructionExposesScaledStUntilFwait) {
    m_cpu->fpu.control_word = 0x036F; // PC=64, nearest, #U unmasked
    m_cpu->fpu.status_word = 0;
    m_cpu->fpu.tag_word = 0xFFF0;
    m_cpu->fpu.st[0] = Float80::from_extended(
        0x8000000000000000ull, 0x0001u);
    m_cpu->fpu.st[1] = Float80(3.0);

    EXPECT_EQ(execute_bytes({0xD8, 0xF1}), ExecResult::Continue); // FDIV ST0,ST1
    EXPECT_EQ(m_cpu->fpu.status_word, 0x82B0u);
    EXPECT_EQ(m_cpu->fpu.get_st(0).mantissa, 0xAAAAAAAAAAAAAAABull);
    EXPECT_EQ(m_cpu->fpu.get_st(0).exponent_sign, 0x5FFFu);
    EXPECT_EQ(m_cpu->fpu.top(), 0u);
    EXPECT_EQ(m_cpu->fpu.tag_word, 0xFFF0u);

    m_cpu->rip += 2;
    EXPECT_EQ(execute_bytes({0x9B}), ExecResult::Exception); // deferred #U
    EXPECT_EQ(m_cpu->fpu.get_st(0).mantissa, 0xAAAAAAAAAAAAAAABull);
    EXPECT_EQ(m_cpu->fpu.get_st(0).exponent_sign, 0x5FFFu);
}

TEST_F(FpuTest, UnmaskedNarrowingStoreSuppressesMemoryAndFstpPop) {
    constexpr u64 kDestination = 0x2000;
    constexpr u64 kSentinel = 0xA5A55A5AC3C33C3Cull;
    struct Trial {
        Float80 value;
        u16 exception;
        bool m64;
    };
    const std::array<Trial, 4> trials{{
        {Float80::from_extended(0x8000000000000001ull, 0x7FFFu),
         FPU_SW_IE, false},
        {Float80::from_extended(0xFFFFFFFFFFFFFFFFull, 0x7FFEu),
         FPU_SW_OE, false},
        {Float80::from_extended(0x8000000000000000ull, 0x0001u),
         FPU_SW_UE, false},
        {Float80::from_extended(0x8000000000000400ull, 0x3FFFu),
         FPU_SW_PE, true},
    }};

    for (const Trial& trial : trials) {
        SCOPED_TRACE(::testing::Message()
                     << "exception=0x" << std::hex << trial.exception
                     << " width=" << std::dec << (trial.m64 ? 64 : 32));
        m_cpu->fpu.reset();
        m_cpu->rip = 0x1000;
        m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), kDestination);
        m_cpu->fpu.control_word = static_cast<u16>(0x037Fu & ~trial.exception);
        m_cpu->fpu.tag_word = 0xFFFCu;
        m_cpu->fpu.st[0] = trial.value;
        ASSERT_TRUE(m_memory->write_u64(kDestination, kSentinel).is_ok());

        const ExecResult result = trial.m64
            ? execute_bytes({0xDD, 0x1B})  // FSTP m64 [RBX]
            : execute_bytes({0xD9, 0x1B}); // FSTP m32 [RBX]
        EXPECT_EQ(result, ExecResult::Continue);
        EXPECT_EQ(m_memory->read_u64(kDestination).value(), kSentinel);
        EXPECT_EQ(m_cpu->fpu.top(), 0u);
        EXPECT_EQ(m_cpu->fpu.tag_word, 0xFFFCu);
        EXPECT_NE(m_cpu->fpu.status_word & trial.exception, 0u);
        EXPECT_NE(m_cpu->fpu.status_word & FPU_SW_ES, 0u);
        EXPECT_NE(m_cpu->fpu.status_word & FPU_SW_B, 0u);
    }
}

TEST_F(FpuTest, DenormalOperandMaskedComputesAndSetsOnlyDe) {
    // Smallest positive double-extended subnormal. This is an operand-class
    // exception (#D), not a tiny-result underflow (#U).
    m_cpu->fpu.control_word = 0x037F; // all exceptions masked
    m_cpu->fpu.status_word = 0;
    m_cpu->fpu.tag_word = 0xFFFC; // physical ST0 occupied
    m_cpu->fpu.st[0] = Float80::from_extended(1, 0);

    EXPECT_EQ(execute_bytes({0xD8, 0xC0}), ExecResult::Continue); // FADD ST0,ST0
    EXPECT_EQ(m_cpu->fpu.status_word & 0x80BFu, FPU_SW_DE);
    EXPECT_EQ(m_cpu->fpu.get_st(0).mantissa, 2u);
    EXPECT_EQ(m_cpu->fpu.get_st(0).exponent_sign, 0u);
}

TEST_F(FpuTest, DenormalOperandUnmaskedDefersAndSuppressesArithmeticCommit) {
    m_cpu->fpu.control_word = 0x037D; // denormal operand unmasked
    m_cpu->fpu.status_word = 0;
    m_cpu->fpu.tag_word = 0xFFFC;
    m_cpu->fpu.st[0] = Float80::from_extended(1, 0);
    const FpuState before = m_cpu->fpu;

    EXPECT_EQ(execute_bytes({0xD8, 0xC0}), ExecResult::Continue); // producer
    EXPECT_EQ(m_cpu->fpu.status_word,
              static_cast<u16>(FPU_SW_B | FPU_SW_ES | FPU_SW_DE));
    EXPECT_EQ(m_cpu->fpu.tag_word, before.tag_word);
    EXPECT_EQ(std::memcmp(m_cpu->fpu.st, before.st, sizeof(before.st)), 0);

    m_cpu->rip += 2;
    const u64 waiting_rip = m_cpu->rip;
    EXPECT_EQ(execute_bytes({0x9B}), ExecResult::Exception); // FWAIT
    EXPECT_EQ(m_cpu->rip, waiting_rip);
}

TEST_F(FpuTest, DenormalCompareHonorsMaskAndPreservesFlagsWhenUnmasked) {
    const Float80 denormal = Float80::from_extended(1, 0);
    m_cpu->fpu.tag_word = 0xFFFC;
    m_cpu->fpu.st[0] = denormal;

    m_cpu->fpu.control_word = 0x037F;
    m_cpu->fpu.status_word = 0;
    EXPECT_EQ(execute_bytes({0xD8, 0xD0}), ExecResult::Continue); // FCOM ST0
    EXPECT_EQ(m_cpu->fpu.status_word,
              static_cast<u16>(FPU_SW_DE | FPU_SW_C3));

    m_cpu->rip += 2;
    m_cpu->fpu.control_word = 0x037D;
    m_cpu->fpu.status_word = FPU_SW_C0;
    EXPECT_EQ(execute_bytes({0xD8, 0xD0}), ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.status_word,
              static_cast<u16>(FPU_SW_B | FPU_SW_ES | FPU_SW_DE | FPU_SW_C0));
}

TEST_F(FpuTest, IeeeMemoryDenormalRaisesDeBeforeWidening) {
    constexpr u64 kSource = 0x2000;
    ASSERT_TRUE(m_memory->write_u64(kSource, 1).is_ok()); // min binary64 subnormal
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), kSource);

    m_cpu->fpu.control_word = 0x037F;
    EXPECT_EQ(execute_bytes({0xDD, 0x03}), ExecResult::Continue); // FLD m64
    EXPECT_EQ(m_cpu->fpu.status_word & 0x80BFu, FPU_SW_DE);
    EXPECT_EQ(m_cpu->fpu.top(), 7u);
    EXPECT_FALSE(m_cpu->fpu.get_st(0).is_denormal())
        << "binary64 subnormal widens to a normal extended value";

    m_cpu->rip += 2;
    m_cpu->fpu.reset();
    m_cpu->fpu.control_word = 0x037D;
    m_cpu->fpu.tag_word = 0xFFFC;
    m_cpu->fpu.st[0] = Float80(1.0);
    EXPECT_EQ(execute_bytes({0xDC, 0x03}), ExecResult::Continue); // FADD m64
    EXPECT_EQ(m_cpu->fpu.status_word, 0x8082u);
    EXPECT_DOUBLE_EQ(m_cpu->fpu.get_st(0).to_double(), 1.0);
}

TEST_F(FpuTest, PendingUnmaskedStackOverflowFaultsWaitingArithmeticFirst) {
    m_cpu->fpu.control_word = 0x027E;
    m_cpu->fpu.tag_word = 0x0000;
    for (u8 i = 0; i < 8; ++i) m_cpu->fpu.st[i] = Float80(double(i + 1));

    EXPECT_EQ(execute_bytes({0xD9, 0xEE}), ExecResult::Continue); // FLDZ
    const FpuState pending = m_cpu->fpu;
    m_cpu->rip += 2;
    EXPECT_EQ(execute_bytes({0xD8, 0xC0}), ExecResult::Exception); // FADD ST0,ST0
    EXPECT_EQ(std::memcmp(&m_cpu->fpu, &pending, sizeof(FpuState)), 0)
        << "waiting arithmetic must fault before clearing C1 or changing ST0";
}

TEST_F(FpuTest, FnclexAndFninitDiscardPendingExceptionWithoutWaiting) {
    const auto seed_pending = [&] {
        m_cpu->fpu.control_word = 0x027E;
        m_cpu->fpu.status_word = 0x82C1;
        m_cpu->fpu.tag_word = 0x0000;
        m_cpu->fpu.st[0] = Float80(1.0);
    };

    seed_pending();
    EXPECT_EQ(execute_bytes({0xDB, 0xE2}), ExecResult::Continue); // FNCLEX
    EXPECT_EQ(m_cpu->fpu.status_word, 0x0200u); // C1 preserved, pending cleared

    seed_pending();
    EXPECT_EQ(execute_bytes({0xDB, 0xE3}), ExecResult::Continue); // FNINIT
    EXPECT_EQ(m_cpu->fpu.control_word, 0x037Fu);
    EXPECT_EQ(m_cpu->fpu.status_word, 0u);
    EXPECT_EQ(m_cpu->fpu.tag_word, 0xFFFFu);
}

TEST_F(FpuTest, FnstswIgnoresPendingButFnopDeliversSpecificException) {
    m_cpu->fpu.control_word = 0x037B; // divide-by-zero unmasked
    m_cpu->fpu.status_word = FPU_SW_ES | FPU_SW_ZE;

    EXPECT_EQ(execute_bytes({0xDF, 0xE0}), ExecResult::Continue); // FNSTSW AX
    EXPECT_EQ(m_cpu->gpr.get16(static_cast<u8>(RegIndex::RAX)),
              FPU_SW_ES | FPU_SW_ZE);
    m_cpu->rip += 2;
    EXPECT_EQ(execute_bytes({0xD9, 0xD0}), ExecResult::Exception); // FNOP waits

    m_cpu->fpu.control_word = 0x037F; // same sticky flag now masked
    m_cpu->rip += 2;
    EXPECT_EQ(execute_bytes({0xD9, 0xD0}), ExecResult::Continue);
}

// 15. FLDCW / FNSTCW round-trip
TEST_F(FpuTest, FldcwFnstcwRoundTrip) {
    const u64 data_addr = 0x2000;
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), data_addr);

    // Write a custom control word to memory
    (void)m_memory->write_u16(data_addr, 0x027F); // Single precision, round-to-nearest

    // FLDCW [RBX]: D9 2B
    auto r1 = execute_bytes({0xD9, 0x2B});
    EXPECT_EQ(r1, ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.control_word, 0x027F);

    // Now store it back to a different location
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), data_addr + 0x10);

    // FNSTCW [RBX]: D9 3B
    auto r2 = execute_bytes({0xD9, 0x3B});
    EXPECT_EQ(r2, ExecResult::Continue);
    auto stored = m_memory->read_u16(data_addr + 0x10);
    EXPECT_EQ(stored.value(), 0x027F);
}

TEST_F(FpuTest, FldcwUnmaskMakesStickyExceptionPendingForNextWait) {
    constexpr u64 kControl = 0x2000;
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), kControl);
    m_cpu->fpu.control_word = 0x037F; // divide-by-zero masked
    m_cpu->fpu.status_word = FPU_SW_ZE; // sticky, but ES/B clear
    m_cpu->flags.rflags = FLAG_CF | FLAG_ZF | 0x2u;
    ASSERT_TRUE(m_memory->write_u16(kControl, 0x037B).is_ok());

    EXPECT_EQ(execute_bytes({0xD9, 0x2B}), ExecResult::Continue); // FLDCW [RBX]
    EXPECT_EQ(m_cpu->fpu.control_word, 0x037Bu);
    EXPECT_EQ(m_cpu->fpu.status_word,
              static_cast<u16>(FPU_SW_B | FPU_SW_ES | FPU_SW_ZE));
    EXPECT_EQ(m_cpu->flags.rflags, FLAG_CF | FLAG_ZF | 0x2u);

    m_cpu->rip += 2;
    EXPECT_EQ(execute_bytes({0x9B}), ExecResult::Exception); // FWAIT
    EXPECT_EQ(m_cpu->fpu.status_word,
              static_cast<u16>(FPU_SW_B | FPU_SW_ES | FPU_SW_ZE));
    EXPECT_EQ(m_cpu->flags.rflags, FLAG_CF | FLAG_ZF | 0x2u);
}

// 16. FNSTSW AX
TEST_F(FpuTest, FnstswAx) {
    m_cpu->fpu.status_word = 0x4100; // C0 set + something

    // FNSTSW AX: DF E0
    auto r = execute_bytes({0xDF, 0xE0});
    EXPECT_EQ(r, ExecResult::Continue);
    u16 ax = m_cpu->gpr.get16(static_cast<u8>(RegIndex::RAX));
    EXPECT_EQ(ax, 0x4100);
}

// 17. FTST checks ST(0) against zero
TEST_F(FpuTest, Ftst) {
    m_cpu->fpu.push(Float80(5.0));

    // FTST: D9 E4 — ST(0)=5.0 > 0 → C3=0,C2=0,C0=0
    auto r = execute_bytes({0xD9, 0xE4});
    EXPECT_EQ(r, ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.status_word & FPU_SW_C0, 0);
    EXPECT_EQ(m_cpu->fpu.status_word & FPU_SW_C3, 0);

    // Negative value
    m_cpu->fpu.set_st(0, Float80(-1.0));
    auto r2 = execute_bytes({0xD9, 0xE4});
    EXPECT_EQ(r2, ExecResult::Continue);
    // ST(0) < 0 → C0=1
    EXPECT_NE(m_cpu->fpu.status_word & FPU_SW_C0, 0);
    EXPECT_EQ(m_cpu->fpu.status_word & FPU_SW_C3, 0);
}

TEST_F(FpuTest, SqrtExactPowersOfFourPreserveStatusAcrossPrecisionModes) {
    // Intel SDM Vol. 2 FSQRT: exact results do not raise #P and therefore do
    // not set C1 as a rounding-direction indicator. Exercise every PC and RC
    // encoding, including FNINIT's extended-precision default.
    struct ExactCase {
        double input;
        u16 result_exp;
    };
    constexpr ExactCase cases[] = {
        {0.25, 0x3FFEu},  // sqrt(2^-2) = 2^-1
        {4.0, 0x4000u},   // sqrt(2^2)  = 2^1
        {16.0, 0x4001u},  // sqrt(2^4)  = 2^2
    };
    for (const ExactCase& c : cases) {
        for (u16 pc : {u16{0x0000}, u16{0x0200}, u16{0x0300}}) {
            for (u16 rc : {u16{0x0000}, u16{0x0400}, u16{0x0800},
                           u16{0x0C00}}) {
                m_cpu->fpu.control_word =
                    static_cast<u16>(0x007F | pc | rc);
                m_cpu->fpu.status_word = FPU_SW_C0;

                const Float80 result =
                    m_cpu->fpu.sqrt_x87(Float80(c.input));

                EXPECT_EQ(result.mantissa, 0x8000000000000000ull)
                    << "input=" << c.input << " pc=" << pc << " rc=" << rc;
                EXPECT_EQ(result.exponent_sign, c.result_exp)
                    << "input=" << c.input << " pc=" << pc << " rc=" << rc;
                EXPECT_EQ(m_cpu->fpu.status_word, FPU_SW_C0)
                    << "input=" << c.input << " pc=" << pc << " rc=" << rc;
            }
        }
    }

    // An odd power-of-two exponent is not an exact square and must retain the
    // normal correctly-rounded path, including its precision exception.
    m_cpu->fpu.control_word = 0x037F;
    m_cpu->fpu.status_word = 0;
    (void)m_cpu->fpu.sqrt_x87(Float80(2.0));
    EXPECT_NE(m_cpu->fpu.status_word & FPU_SW_PE, 0);
}

// 18. FLD ST(i) — duplicate
TEST_F(FpuTest, FldSti) {
    m_cpu->fpu.push(Float80(77.0));
    // Stack: ST(0) = 77.0

    // FLD ST(0): D9 C0 — pushes copy of ST(0)
    auto r = execute_bytes({0xD9, 0xC0});
    EXPECT_EQ(r, ExecResult::Continue);
    EXPECT_DOUBLE_EQ(m_cpu->fpu.get_st(0).to_double(), 77.0);
    EXPECT_DOUBLE_EQ(m_cpu->fpu.get_st(1).to_double(), 77.0);
}

// 19. FSTP ST(i)
TEST_F(FpuTest, FstpSti) {
    m_cpu->fpu.push(Float80(100.0)); // ST(2) after two more pushes
    m_cpu->fpu.push(Float80(200.0)); // ST(1)
    m_cpu->fpu.push(Float80(300.0)); // ST(0)

    // FSTP ST(2): DD D2+i where i=2 → DD DA
    auto r = execute_bytes({0xDD, 0xDA});
    EXPECT_EQ(r, ExecResult::Continue);
    // ST(0) was 300 and gets popped; old ST(2)=100 is overwritten with 300
    // After pop: new ST(0)=200, new ST(1)=300
    EXPECT_DOUBLE_EQ(m_cpu->fpu.get_st(0).to_double(), 200.0);
    EXPECT_DOUBLE_EQ(m_cpu->fpu.get_st(1).to_double(), 300.0);
}

TEST_F(FpuTest, FstStiCopiesWithoutPopping) {
    m_cpu->fpu.push(Float80(100.0));
    m_cpu->fpu.push(Float80(200.0));
    m_cpu->fpu.push(Float80(300.0));

    // FST ST(1): DD D1 — copy ST(0) into ST(1), preserving TOP.
    auto r = execute_bytes({0xDD, 0xD1});
    EXPECT_EQ(r, ExecResult::Continue);
    EXPECT_DOUBLE_EQ(m_cpu->fpu.get_st(0).to_double(), 300.0);
    EXPECT_DOUBLE_EQ(m_cpu->fpu.get_st(1).to_double(), 300.0);
    EXPECT_DOUBLE_EQ(m_cpu->fpu.get_st(2).to_double(), 100.0);
}

// 20. FILD m64 + FISTP m64
TEST_F(FpuTest, FildFistpM64) {
    const u64 data_addr = 0x2000;
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), data_addr);
    write_i64(data_addr, 123456789LL);

    // FILD m64 [RBX]: DF /5 — DF 2B
    auto r1 = execute_bytes({0xDF, 0x2B});
    EXPECT_EQ(r1, ExecResult::Continue);
    EXPECT_DOUBLE_EQ(m_cpu->fpu.get_st(0).to_double(), 123456789.0);

    // FISTP m64 [RBX]: DF /7 — DF 3B
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), data_addr + 0x10);
    auto r2 = execute_bytes({0xDF, 0x3B});
    EXPECT_EQ(r2, ExecResult::Continue);
    EXPECT_EQ(read_i64(data_addr + 0x10), 123456789LL);
}

// Intel SDM Vol. 2 FILD/FISTP: every signed 64-bit integer is exact in
// double-extended precision, so the pair restores all 64 bits. A binary64
// intermediate drops the low bits of any |n| > 2^53.
TEST_F(FpuTest, FildFistpM64RoundTripsIntegersPastBinary64) {
    const u64 data_addr = 0x2000;
    m_cpu->fpu.control_word = 0x037F;
    const u64 values[] = {
        0x0055005C003A0043ULL,
        0x0020000000000001ULL,
        0x7FFFFFFFFFFFFFFFULL,
        0x8000000000000000ULL,
        0xFFFFFFFFFFFFFFFFULL,
    };
    for (u64 bits : values) {
        m_cpu->fpu.reset();
        m_cpu->fpu.control_word = 0x037F;
        write_i64(data_addr, static_cast<i64>(bits));
        m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), data_addr);
        ASSERT_EQ(execute_bytes({0xDF, 0x2B}), ExecResult::Continue);
        m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), data_addr + 0x10);
        ASSERT_EQ(execute_bytes({0xDF, 0x3B}), ExecResult::Continue);
        EXPECT_EQ(static_cast<u64>(read_i64(data_addr + 0x10)), bits);
        EXPECT_EQ(m_cpu->fpu.status_word & FPU_SW_PE, 0);
    }

    // Nearest-even, precision masked: 1.5 → 2 (up), 2.5 → 2 (even).
    m_cpu->fpu.reset();
    m_cpu->fpu.control_word = 0x037F;
    m_cpu->fpu.push(Float80(1.5));
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), data_addr);
    ASSERT_EQ(execute_bytes({0xDF, 0x3B}), ExecResult::Continue);
    EXPECT_EQ(read_i64(data_addr), 2);
    EXPECT_NE(m_cpu->fpu.status_word & FPU_SW_C1, 0);

    m_cpu->fpu.reset();
    m_cpu->fpu.control_word = 0x037F;
    m_cpu->fpu.push(Float80(2.5));
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), data_addr);
    ASSERT_EQ(execute_bytes({0xDF, 0x3B}), ExecResult::Continue);
    EXPECT_EQ(read_i64(data_addr), 2);
    EXPECT_EQ(m_cpu->fpu.status_word & FPU_SW_C1, 0);
}

// 21. FADD m32
TEST_F(FpuTest, FaddM32) {
    const u64 data_addr = 0x2000;
    m_cpu->fpu.push(Float80(10.0));
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), data_addr);
    write_f32(data_addr, 5.0f);

    // FADD m32 [RBX]: D8 03
    auto r = execute_bytes({0xD8, 0x03});
    EXPECT_EQ(r, ExecResult::Continue);
    EXPECT_NEAR(m_cpu->fpu.get_st(0).to_double(), 15.0, 1e-5);
}

// 22. FDECSTP / FINCSTP
TEST_F(FpuTest, FdecstpFincstp) {
    u8 orig_top = m_cpu->fpu.top();

    // FDECSTP: D9 F6
    auto r1 = execute_bytes({0xD9, 0xF6});
    EXPECT_EQ(r1, ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.top(), (orig_top - 1) & 7);

    // FINCSTP: D9 F7
    auto r2 = execute_bytes({0xD9, 0xF7});
    EXPECT_EQ(r2, ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.top(), orig_top);
}

// =============================================================================
// x87 transcendental block (D9 F0..F9)
//
// None of these decoded before; FYL2X in particular is how a guest CRT builds
// log() and pow(), and Battle.net-Setup.exe raised InvalidOpcode on `d9 f1`
// while laying out its language screen.
// =============================================================================

TEST_F(FpuTest, Fyl2xComputesYTimesLog2X) {
    // FLD 3.0 (y) ; FLD 8.0 (x) ; FYL2X  ->  3 * log2(8) == 9
    const u64 a = 0x2000, b = 0x2008;
    write_f64(a, 3.0);
    write_f64(b, 8.0);
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), a);
    ASSERT_EQ(execute_bytes({0xDD, 0x03}), ExecResult::Continue);   // FLD m64 [RBX]
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), b);
    ASSERT_EQ(execute_bytes({0xDD, 0x03}), ExecResult::Continue);
    ASSERT_EQ(execute_bytes({0xD9, 0xF1}), ExecResult::Continue);   // FYL2X
    EXPECT_NEAR(m_cpu->fpu.get_st(0).to_double(), 9.0, 1e-9);
}

TEST_F(FpuTest, F2xm1ComputesTwoToTheXMinusOne) {
    const u64 a = 0x2000;
    write_f64(a, 0.5);
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), a);
    ASSERT_EQ(execute_bytes({0xDD, 0x03}), ExecResult::Continue);
    ASSERT_EQ(execute_bytes({0xD9, 0xF0}), ExecResult::Continue);   // F2XM1
    EXPECT_NEAR(m_cpu->fpu.get_st(0).to_double(), std::exp2(0.5) - 1.0, 1e-9);
}

TEST_F(FpuTest, FpatanComputesAtan2) {
    // FLD 1.0 (y) ; FLD 1.0 (x) ; FPATAN -> atan2(1,1) == pi/4
    const u64 a = 0x2000;
    write_f64(a, 1.0);
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), a);
    ASSERT_EQ(execute_bytes({0xDD, 0x03}), ExecResult::Continue);
    ASSERT_EQ(execute_bytes({0xDD, 0x03}), ExecResult::Continue);
    ASSERT_EQ(execute_bytes({0xD9, 0xF3}), ExecResult::Continue);   // FPATAN
    EXPECT_NEAR(m_cpu->fpu.get_st(0).to_double(), std::atan2(1.0, 1.0), 1e-9);
}

TEST_F(FpuTest, Fyl2xp1ComputesYTimesLog2XPlus1) {
    // FLD 2.0 (y) ; FLD 3.0 (x) ; FYL2XP1 -> 2 * log2(4) == 4
    const u64 a = 0x2000, b = 0x2008;
    write_f64(a, 2.0);
    write_f64(b, 3.0);
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), a);
    ASSERT_EQ(execute_bytes({0xDD, 0x03}), ExecResult::Continue);
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), b);
    ASSERT_EQ(execute_bytes({0xDD, 0x03}), ExecResult::Continue);
    ASSERT_EQ(execute_bytes({0xD9, 0xF9}), ExecResult::Continue);   // FYL2XP1
    EXPECT_NEAR(m_cpu->fpu.get_st(0).to_double(), 4.0, 1e-9);
}

TEST_F(FpuTest, FxtractSplitsExponentAndSignificand) {
    // 12.0 == 1.5 * 2^3  ->  ST(1)=exponent 3, ST(0)=significand 1.5
    const u64 a = 0x2000;
    write_f64(a, 12.0);
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), a);
    ASSERT_EQ(execute_bytes({0xDD, 0x03}), ExecResult::Continue);
    ASSERT_EQ(execute_bytes({0xD9, 0xF4}), ExecResult::Continue);   // FXTRACT
    EXPECT_NEAR(m_cpu->fpu.get_st(0).to_double(), 1.5, 1e-9);
    EXPECT_NEAR(m_cpu->fpu.get_st(1).to_double(), 3.0, 1e-9);
}

TEST_F(FpuTest, FpremComputesTruncatedRemainder) {
    // FLD 3.0 (divisor -> ST(1)) ; FLD 8.0 (ST(0)) ; FPREM -> fmod(8,3) == 2
    const u64 a = 0x2000, b = 0x2008;
    write_f64(a, 3.0);
    write_f64(b, 8.0);
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), a);
    ASSERT_EQ(execute_bytes({0xDD, 0x03}), ExecResult::Continue);
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), b);
    ASSERT_EQ(execute_bytes({0xDD, 0x03}), ExecResult::Continue);
    ASSERT_EQ(execute_bytes({0xD9, 0xF8}), ExecResult::Continue);   // FPREM
    EXPECT_NEAR(m_cpu->fpu.get_st(0).to_double(), 2.0, 1e-9);
}

TEST_F(FpuTest, Fprem1ComputesIeeeRemainder) {
    // remainder(8,3) rounds the quotient to nearest: 8 - 3*3 == -1
    const u64 a = 0x2000, b = 0x2008;
    write_f64(a, 3.0);
    write_f64(b, 8.0);
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), a);
    ASSERT_EQ(execute_bytes({0xDD, 0x03}), ExecResult::Continue);
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), b);
    ASSERT_EQ(execute_bytes({0xDD, 0x03}), ExecResult::Continue);
    ASSERT_EQ(execute_bytes({0xD9, 0xF5}), ExecResult::Continue);   // FPREM1
    EXPECT_NEAR(m_cpu->fpu.get_st(0).to_double(), -1.0, 1e-9);
}

TEST_F(FpuTest, FptanPushesOneAfterTangent) {
    const u64 a = 0x2000;
    write_f64(a, 0.5);
    m_cpu->gpr.set64(static_cast<u8>(RegIndex::RBX), a);
    ASSERT_EQ(execute_bytes({0xDD, 0x03}), ExecResult::Continue);
    ASSERT_EQ(execute_bytes({0xD9, 0xF2}), ExecResult::Continue);   // FPTAN
    // x87 pushes 1.0 so the caller can treat the result as a ratio.
    EXPECT_NEAR(m_cpu->fpu.get_st(0).to_double(), 1.0, 1e-12);
    EXPECT_NEAR(m_cpu->fpu.get_st(1).to_double(), std::tan(0.5), 1e-9);
}

// ============================================================================
// x86 NaN semantics (verified against the Windows real-silicon box)
//
// These were 165 of the 166 divergences found between Vitreo and real x86.
// ARM produces a POSITIVE default quiet NaN and discards the incoming
// payload; x86 does neither. Because both the interpreter and the JIT's
// native paths inherit host arithmetic, they agreed with each other while
// both differed from silicon -- so a JIT-vs-interpreter parity test cannot
// catch this, and only real x86 hardware can.
// ============================================================================

namespace {

u64 bits_of(double d) {
    u64 v;
    std::memcpy(&v, &d, sizeof(v));
    return v;
}

double double_from(u64 v) {
    double d;
    std::memcpy(&d, &v, sizeof(d));
    return d;
}

} // namespace

// Intel SDM Vol. 1 4.8.3.7: an invalid operation with no NaN operand yields
// the QNaN floating-point indefinite, whose SIGN BIT IS SET. Silicon returns
// FFF8000000000000 for 0/0 where Vitreo used to return 7FF8000000000000.
TEST_F(FpuTest, InvalidOperationYieldsNegativeIndefiniteQnan) {
    constexpr u64 kIndefinite = 0xFFF8000000000000ULL;

    EXPECT_EQ(bits_of(x87_arith(0.0, 0.0, X87Op::Div)), kIndefinite)
        << "0/0 must be the negative indefinite, not a positive default NaN";
    EXPECT_EQ(bits_of(x87_arith(0.0, INFINITY, X87Op::Mul)), kIndefinite)
        << "0*inf is an invalid operation";
    EXPECT_EQ(bits_of(x87_arith(INFINITY, INFINITY, X87Op::Sub)), kIndefinite)
        << "inf-inf is an invalid operation";
    EXPECT_EQ(bits_of(x87_arith(INFINITY, INFINITY, X87Op::Div)), kIndefinite)
        << "inf/inf is an invalid operation";
}

// Intel SDM Vol. 1 4.8.3.4: a NaN operand propagates, quieted, with its
// payload intact -- it is not replaced by a default NaN.
TEST_F(FpuTest, NanOperandPropagatesWithPayloadPreserved) {
    // The payload real silicon produces when float 7F800001 is widened.
    const double snan = double_from(0x7FF0000020000000ULL);

    for (const auto op : {X87Op::Add, X87Op::Sub, X87Op::Mul, X87Op::Div}) {
        EXPECT_EQ(bits_of(x87_arith(1.0, snan, op)), 0x7FF8000020000000ULL)
            << "payload must survive, with the quiet bit forced on";
        EXPECT_EQ(bits_of(x87_arith(snan, 1.0, op)), 0x7FF8000020000000ULL)
            << "propagation must not depend on operand order";
    }
}

// With two NaN operands x87 returns the one with the larger significand.
TEST_F(FpuTest, TwoNanOperandsKeepTheLargerSignificand) {
    const double small = double_from(0x7FF0000000000001ULL);
    const double large = double_from(0x7FF4000000000000ULL);

    EXPECT_EQ(bits_of(x87_arith(small, large, X87Op::Add)),
              0x7FFC000000000000ULL);
    EXPECT_EQ(bits_of(x87_arith(large, small, X87Op::Add)),
              0x7FFC000000000000ULL);
}

// Float80 is the x87 register format, so a NaN must survive the round trip
// through it. to_double() used to rebuild a bare quiet NaN and throw the
// payload away, which is how the payload was lost even after the arithmetic
// itself was correct.
TEST_F(FpuTest, Float80RoundTripPreservesNanPayload) {
    const double nan_with_payload = double_from(0x7FF8000020000000ULL);
    EXPECT_EQ(bits_of(Float80(nan_with_payload).to_double()),
              0x7FF8000020000000ULL);

    const double negative_indefinite = double_from(0xFFF8000000000000ULL);
    EXPECT_EQ(bits_of(Float80(negative_indefinite).to_double()),
              0xFFF8000000000000ULL)
        << "the indefinite's sign must survive the register format";

    // Infinity must stay infinity -- it is not a NaN and has no payload.
    EXPECT_EQ(bits_of(Float80(INFINITY).to_double()), 0x7FF0000000000000ULL);
    EXPECT_EQ(bits_of(Float80(-INFINITY).to_double()), 0xFFF0000000000000ULL);
}
