// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// SSE4.1/SSE4.2 instruction tests (CPU-041)

#include "core/math/host_fenv.hpp"
#include <gtest/gtest.h>
#include <cstring>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/jit/jit_compiler.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

class Sse4Test : public ::testing::Test {
protected:
    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        auto map_result = m_memory->map_region(0x1000, 0x20000,
                                                MemoryProtection::ReadWrite);
        ASSERT_TRUE(map_result);
    }

    ExecResult run_code(const u8* code, usize len, CpuState& cpu,
                        DecodeMode mode = DecodeMode::Mode32) {
        EXPECT_TRUE(m_memory->write_bytes(0x1000, code, len));
        cpu.rip = 0x1000;

        MemoryByteReader reader(code, len);
        u32 count = 0;
        while (reader.remaining() > 0) {
            DecodedInstruction inst;
            if (decode_instruction(reader, mode, inst)
                != DecodeError::Success) break;
            ++count;
        }
        Interpreter interp(cpu, *m_memory);
        return interp.run(count);
    }

    void init_cpu(CpuState& cpu) {
        cpu.init_protected_mode();
        cpu.gpr.set32(static_cast<u8>(RegIndex::RSP), 0x10000);
    }

    std::unique_ptr<GuestMemory> m_memory;
};

// ============================================================================
// PTEST
// ============================================================================

TEST_F(Sse4Test, PtestAllZero) {
    CpuState cpu;
    init_cpu(cpu);
    // XMM0 = all zeros, XMM1 = all ones
    cpu.sse.xmm[0].clear();
    cpu.sse.xmm[1].u64_[0] = ~0ULL;
    cpu.sse.xmm[1].u64_[1] = ~0ULL;
    // PTEST XMM0, XMM1  => AND=0 so ZF=1, ANDN=~0 & 1 =1 so CF=0
    const u8 code[] = { 0x66, 0x0F, 0x38, 0x17, 0xC1 };
    auto result = run_code(code, sizeof(code), cpu);
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_TRUE(cpu.flags.zf());
    EXPECT_FALSE(cpu.flags.cf());
}

TEST_F(Sse4Test, PtestCfSet) {
    CpuState cpu;
    init_cpu(cpu);
    // XMM0 = all ones, XMM1 = some value
    cpu.sse.xmm[0].u64_[0] = ~0ULL;
    cpu.sse.xmm[0].u64_[1] = ~0ULL;
    cpu.sse.xmm[1].u64_[0] = 0x1234;
    cpu.sse.xmm[1].u64_[1] = 0;
    // PTEST XMM0, XMM1  => AND=0x1234!=0 ZF=0, ANDN= ~all_ones & src = 0 CF=1
    const u8 code[] = { 0x66, 0x0F, 0x38, 0x17, 0xC1 };
    auto result = run_code(code, sizeof(code), cpu);
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_FALSE(cpu.flags.zf());
    EXPECT_TRUE(cpu.flags.cf());
}

// ============================================================================
// PMOVZXBW
// ============================================================================

TEST_F(Sse4Test, PmovzxbwBasic) {
    CpuState cpu;
    init_cpu(cpu);
    cpu.sse.xmm[1].clear();
    for (int i = 0; i < 8; ++i)
        cpu.sse.xmm[1].u8_[i] = static_cast<u8>(i + 10);
    // PMOVZXBW XMM0, XMM1  => 66 0F 38 30 C1
    const u8 code[] = { 0x66, 0x0F, 0x38, 0x30, 0xC1 };
    auto result = run_code(code, sizeof(code), cpu);
    EXPECT_EQ(result, ExecResult::Continue);
    for (int i = 0; i < 8; ++i)
        EXPECT_EQ(cpu.sse.xmm[0].u16_[i], static_cast<u16>(i + 10));
}

// PMOVZXDQ with a 64-bit memory operand whose 8 bytes end exactly at the tail
// of the mapped region (next page unmapped). The instruction reads only m64, so
// it must NOT fault — regression for the blanket 16-byte read_pack over-read
// that AV'd on libcef's RIP-relative GHASH constant (Steam webhelper).
TEST_F(Sse4Test, PmovzxdqM64AtMappedTailDoesNotOverread) {
    CpuState cpu;
    init_cpu(cpu);
    // Region is [0x1000, 0x21000); place the qword in the last 8 bytes so any
    // read past 8 crosses into the unmapped page at 0x21000.
    constexpr u32 src = 0x21000 - 8;  // 0x20FF8
    ASSERT_TRUE(m_memory->write_u64(src, 0x1122334455667788ull));

    // PMOVZXDQ XMM0, [disp32]  => 66 0F 38 35 /r, ModRM 05 = [disp32] (32-bit).
    const u8 code[] = {
        0x66, 0x0F, 0x38, 0x35, 0x05,
        static_cast<u8>(src & 0xFF), static_cast<u8>((src >> 8) & 0xFF),
        static_cast<u8>((src >> 16) & 0xFF), static_cast<u8>((src >> 24) & 0xFF)
    };
    auto result = run_code(code, sizeof(code), cpu);
    ASSERT_EQ(result, ExecResult::Continue) << "pmovzxdq m64 must not fault";
    // Zero-extend the two low dwords of the source qword.
    EXPECT_EQ(cpu.sse.xmm[0].u64_[0], 0x55667788ull);
    EXPECT_EQ(cpu.sse.xmm[0].u64_[1], 0x11223344ull);
}

// ============================================================================
// PMOVSXBD
// ============================================================================

TEST_F(Sse4Test, PmovsxbdNegative) {
    CpuState cpu;
    init_cpu(cpu);
    cpu.sse.xmm[1].clear();
    cpu.sse.xmm[1].u8_[0] = 0xFF;  // -1
    cpu.sse.xmm[1].u8_[1] = 0x80;  // -128
    cpu.sse.xmm[1].u8_[2] = 0x01;  //  1
    cpu.sse.xmm[1].u8_[3] = 0x7F;  //  127
    // PMOVSXBD XMM0, XMM1  => 66 0F 38 21 C1
    const u8 code[] = { 0x66, 0x0F, 0x38, 0x21, 0xC1 };
    auto result = run_code(code, sizeof(code), cpu);
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(cpu.sse.xmm[0].i32_[0], -1);
    EXPECT_EQ(cpu.sse.xmm[0].i32_[1], -128);
    EXPECT_EQ(cpu.sse.xmm[0].i32_[2], 1);
    EXPECT_EQ(cpu.sse.xmm[0].i32_[3], 127);
}

// ============================================================================
// PMAXUD / PMINUD
// ============================================================================

TEST_F(Sse4Test, PmaxudBasic) {
    CpuState cpu;
    init_cpu(cpu);
    cpu.sse.xmm[0].u32_[0] = 5; cpu.sse.xmm[0].u32_[1] = 100;
    cpu.sse.xmm[0].u32_[2] = 0; cpu.sse.xmm[0].u32_[3] = 0xFFFFFFFF;
    cpu.sse.xmm[1].u32_[0] = 10; cpu.sse.xmm[1].u32_[1] = 50;
    cpu.sse.xmm[1].u32_[2] = 1; cpu.sse.xmm[1].u32_[3] = 0x80000000;
    // PMAXUD XMM0, XMM1  => 66 0F 38 3F C1
    const u8 code[] = { 0x66, 0x0F, 0x38, 0x3F, 0xC1 };
    auto result = run_code(code, sizeof(code), cpu);
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(cpu.sse.xmm[0].u32_[0], 10u);
    EXPECT_EQ(cpu.sse.xmm[0].u32_[1], 100u);
    EXPECT_EQ(cpu.sse.xmm[0].u32_[2], 1u);
    EXPECT_EQ(cpu.sse.xmm[0].u32_[3], 0xFFFFFFFFu);
}

// ============================================================================
// BLENDPS
// ============================================================================

TEST_F(Sse4Test, BlendpsSelect) {
    CpuState cpu;
    init_cpu(cpu);
    cpu.sse.xmm[0].u32_[0] = 1; cpu.sse.xmm[0].u32_[1] = 2;
    cpu.sse.xmm[0].u32_[2] = 3; cpu.sse.xmm[0].u32_[3] = 4;
    cpu.sse.xmm[1].u32_[0] = 10; cpu.sse.xmm[1].u32_[1] = 20;
    cpu.sse.xmm[1].u32_[2] = 30; cpu.sse.xmm[1].u32_[3] = 40;
    // BLENDPS XMM0, XMM1, 0x05  => 66 0F 3A 0C C1 05
    // imm=0b0101 => select lanes 0 and 2 from src
    const u8 code[] = { 0x66, 0x0F, 0x3A, 0x0C, 0xC1, 0x05 };
    auto result = run_code(code, sizeof(code), cpu);
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(cpu.sse.xmm[0].u32_[0], 10u);  // from src
    EXPECT_EQ(cpu.sse.xmm[0].u32_[1], 2u);   // from dst
    EXPECT_EQ(cpu.sse.xmm[0].u32_[2], 30u);  // from src
    EXPECT_EQ(cpu.sse.xmm[0].u32_[3], 4u);   // from dst
}

// ============================================================================
// ROUNDSS
// ============================================================================

TEST_F(Sse4Test, RoundssFloor) {
    CpuState cpu;
    init_cpu(cpu);
    float val = 2.7f;
    std::memcpy(&cpu.sse.xmm[1].u32_[0], &val, 4);
    // ROUNDSS XMM0, XMM1, 1  (floor) => 66 0F 3A 0A C1 01
    const u8 code[] = { 0x66, 0x0F, 0x3A, 0x0A, 0xC1, 0x01 };
    auto result = run_code(code, sizeof(code), cpu);
    EXPECT_EQ(result, ExecResult::Continue);
    float out;
    std::memcpy(&out, &cpu.sse.xmm[0].u32_[0], 4);
    EXPECT_EQ(out, 2.0f);
}

// ============================================================================
// PINSRB / PEXTRB
// ============================================================================

TEST_F(Sse4Test, PinsrbPextrb) {
    CpuState cpu;
    init_cpu(cpu);
    cpu.sse.xmm[0].clear();
    cpu.gpr.set32(static_cast<u8>(RegIndex::RAX), 0x42);
    // PINSRB XMM0, EAX, 5  => 66 0F 3A 20 C0 05
    const u8 pinsrb[] = { 0x66, 0x0F, 0x3A, 0x20, 0xC0, 0x05 };
    auto result = run_code(pinsrb, sizeof(pinsrb), cpu);
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(cpu.sse.xmm[0].u8_[5], 0x42);

    // PEXTRB EAX, XMM0, 5  => 66 0F 3A 14 C0 05
    cpu.gpr.set32(static_cast<u8>(RegIndex::RAX), 0);
    const u8 pextrb[] = { 0x66, 0x0F, 0x3A, 0x14, 0xC0, 0x05 };
    result = run_code(pextrb, sizeof(pextrb), cpu);
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(cpu.gpr.get32(static_cast<u8>(RegIndex::RAX)), 0x42u);
}

// ============================================================================
// CRC32
// ============================================================================

TEST_F(Sse4Test, Crc32Basic) {
    CpuState cpu;
    init_cpu(cpu);
    cpu.gpr.set32(static_cast<u8>(RegIndex::RAX), 0);
    cpu.gpr.set32(static_cast<u8>(RegIndex::RCX), 0x01020304);
    // CRC32 EAX, ECX  => F2 0F 38 F1 C1
    const u8 code[] = { 0xF2, 0x0F, 0x38, 0xF1, 0xC1 };
    auto result = run_code(code, sizeof(code), cpu);
    EXPECT_EQ(result, ExecResult::Continue);
    // CRC32C(0, 0x01020304) should produce a known value
    u32 crc = cpu.gpr.get32(static_cast<u8>(RegIndex::RAX));
    EXPECT_NE(crc, 0u);
}

TEST_F(Sse4Test, Crc32R64Rm8Decode) {
    // F2 REX.W 0F 38 F0 /r : CRC32 r64, r/m8
    const u8 bytes[] = { 0xF2, 0x48, 0x0F, 0x38, 0xF0, 0xC1 };
    MemoryByteReader reader(bytes, sizeof(bytes));
    DecodedInstruction inst;
    ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode64, inst),
              DecodeError::Success);
    EXPECT_EQ(inst.opcode, Opcode::CRC32_R64_RM8);
    ASSERT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 0);  // RAX
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[1].reg, 1);  // CL
}

TEST_F(Sse4Test, Crc32R64Rm8ZeroExtends) {
    CpuState cpu32;
    init_cpu(cpu32);
    cpu32.gpr.set32(static_cast<u8>(RegIndex::RAX), 0);
    cpu32.gpr.set32(static_cast<u8>(RegIndex::RCX), 0x42);
    // CRC32 EAX, CL => F2 0F 38 F0 C1
    const u8 code32[] = { 0xF2, 0x0F, 0x38, 0xF0, 0xC1 };
    ASSERT_EQ(run_code(code32, sizeof(code32), cpu32), ExecResult::Continue);
    const u32 expected = cpu32.gpr.get32(static_cast<u8>(RegIndex::RAX));

    CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.set64(static_cast<u8>(RegIndex::RSP), 0x10000);
    // High bits must be cleared: Intel SDM CRC32 DEST[63-32] := 0.
    cpu.gpr.set64(static_cast<u8>(RegIndex::RAX), 0xFFFFFFFF00000000ULL);
    cpu.gpr.set64(static_cast<u8>(RegIndex::RCX), 0x42);
    // CRC32 RAX, CL => F2 48 0F 38 F0 C1
    const u8 code64[] = { 0xF2, 0x48, 0x0F, 0x38, 0xF0, 0xC1 };
    ASSERT_EQ(run_code(code64, sizeof(code64), cpu, DecodeMode::Mode64),
              ExecResult::Continue);
    EXPECT_EQ(cpu.gpr.get64(static_cast<u8>(RegIndex::RAX)),
              static_cast<u64>(expected));
}

// ============================================================================
// PCMPISTRI — equal-each byte comparison
// ============================================================================

TEST_F(Sse4Test, PcmpistrtEqualEach) {
    CpuState cpu;
    init_cpu(cpu);
    // src1 = "Hello\0...", src2 = "Hello\0..."
    const char* str = "Hello";
    std::memset(&cpu.sse.xmm[0], 0, 16);
    std::memset(&cpu.sse.xmm[1], 0, 16);
    std::memcpy(&cpu.sse.xmm[0], str, 6);
    std::memcpy(&cpu.sse.xmm[1], str, 6);
    // PCMPISTRI XMM0, XMM1, 0x08
    // imm=0x08: byte, equal-each, positive polarity, LSB index
    const u8 code[] = { 0x66, 0x0F, 0x3A, 0x63, 0xC1, 0x08 };
    auto result = run_code(code, sizeof(code), cpu);
    EXPECT_EQ(result, ExecResult::Continue);
    // All bytes match from 0-4 ("Hello"), so bits 0-4 set, LSB index = 0
    u32 ecx = cpu.gpr.get32(static_cast<u8>(RegIndex::RCX));
    EXPECT_EQ(ecx, 0u);
}

// ============================================================================
// PCMPEQQ
// ============================================================================

TEST_F(Sse4Test, PcmpeqqBasic) {
    CpuState cpu;
    init_cpu(cpu);
    cpu.sse.xmm[0].u64_[0] = 0x1234567890ABCDEFULL;
    cpu.sse.xmm[0].u64_[1] = 0xDEADBEEFCAFEBABEULL;
    cpu.sse.xmm[1].u64_[0] = 0x1234567890ABCDEFULL;
    cpu.sse.xmm[1].u64_[1] = 0;
    // PCMPEQQ XMM0, XMM1  => 66 0F 38 29 C1
    const u8 code[] = { 0x66, 0x0F, 0x38, 0x29, 0xC1 };
    auto result = run_code(code, sizeof(code), cpu);
    EXPECT_EQ(result, ExecResult::Continue);
    EXPECT_EQ(cpu.sse.xmm[0].u64_[0], ~0ULL);  // match
    EXPECT_EQ(cpu.sse.xmm[0].u64_[1], 0ULL);   // no match
}

// ============================================================================
// Native JIT vs interpreter: LDMXCSR / STMXCSR / PBLENDVB / BLENDVPS / BLENDVPD
// ============================================================================

namespace {

constexpr GuestAddr kSse4JitCode = 0x1000;
constexpr GuestAddr kSse4JitData = 0x4000;
constexpr u8 kEax = static_cast<u8>(RegIndex::RAX);
constexpr u8 kEsp = static_cast<u8>(RegIndex::RSP);

class ScopedEnvironmentVariable {
public:
    ScopedEnvironmentVariable(const char* name, const char* value)
        : m_name(name) {
        if (const char* previous = std::getenv(name)) {
            m_previous = previous;
        }
        setenv(name, value, 1);
    }

    ~ScopedEnvironmentVariable() {
        if (m_previous) {
            setenv(m_name.c_str(), m_previous->c_str(), 1);
        } else {
            unsetenv(m_name.c_str());
        }
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(
        const ScopedEnvironmentVariable&) = delete;

private:
    std::string m_name;
    std::optional<std::string> m_previous;
};

void seed_blend_regs(CpuState& cpu) {
    // XMM0 mask: mixed per-byte / per-dword / per-qword sign bits.
    cpu.sse.xmm[0].u64_[0] = 0x7F80FF00AABB0080ULL;
    cpu.sse.xmm[0].u64_[1] = 0x800000007FFFFFFFULL;
    // XMM1 dest
    cpu.sse.xmm[1].u64_[0] = 0x1111111111111111ULL;
    cpu.sse.xmm[1].u64_[1] = 0x2222222222222222ULL;
    // XMM2 src
    cpu.sse.xmm[2].u64_[0] = 0xAAAAAAAAAAAAAAAAULL;
    cpu.sse.xmm[2].u64_[1] = 0xBBBBBBBBBBBBBBBBULL;
}

}  // namespace

class Sse4JitParityTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Guest code starts with the host's FP exception flags clear, as the
        // runtime guarantees (run.cpp); the test process's own start-up may
        // have left some set.
        math::clear_host_fp_exceptions();
        m_memory = std::make_unique<GuestMemory>();
        ASSERT_TRUE(m_memory->map_region(
            kSse4JitCode, 0x20000, MemoryProtection::ReadWrite));
    }

    CpuState initial_cpu(bool bits64) const {
        CpuState cpu;
        if (bits64) {
            cpu.init_long_mode();
            cpu.gpr.set64(kEax, kSse4JitData);
            cpu.gpr.set64(kEsp, 0x10000);
        } else {
            cpu.init_protected_mode();
            cpu.gpr.set32(kEax, static_cast<u32>(kSse4JitData));
            cpu.gpr.set32(kEsp, 0x10000);
        }
        cpu.rip = kSse4JitCode;
        return cpu;
    }

    BasicBlock decode_block(const std::vector<u8>& code, bool bits64) {
        BasicBlock block;
        block.start_addr = kSse4JitCode;
        block.is_64bit = bits64;
        MemoryByteReader reader(code.data(), code.size());
        while (reader.remaining() > 0) {
            DecodedInstruction instruction;
            const DecodeError error = decode_instruction(
                reader, bits64 ? DecodeMode::Mode64 : DecodeMode::Mode32,
                instruction);
            EXPECT_EQ(error, DecodeError::Success);
            if (error != DecodeError::Success) {
                break;
            }
            block.instructions.push_back(instruction);
        }
        block.byte_length = static_cast<u32>(reader.position());
        block.instruction_count =
            static_cast<u32>(block.instructions.size());
        return block;
    }

    CpuState run_interpreter(const std::vector<u8>& code, bool bits64,
                             CpuState cpu) {
        EXPECT_TRUE(m_memory->write_bytes(
            kSse4JitCode, code.data(), code.size()));
        Interpreter interpreter(cpu, *m_memory);
        EXPECT_EQ(interpreter.run(static_cast<u32>(block_insn_count(code, bits64))),
                  ExecResult::Continue);
        return cpu;
    }

    CpuState run_native_jit(const std::vector<u8>& code, bool bits64,
                            CpuState cpu) {
        EXPECT_TRUE(m_memory->write_bytes(
            kSse4JitCode, code.data(), code.size()));
        BasicBlock block = decode_block(code, bits64);
        ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
        JitCompiler jit(*m_memory);
        CompiledBlock* compiled = jit.compile(block);
        EXPECT_NE(compiled, nullptr)
            << "native lowering must compile with delegation disabled";
        if (compiled) {
            EXPECT_EQ(jit.execute(*compiled, cpu), JitExecResult::Continue);
        }
        return cpu;
    }

    static u32 block_insn_count(const std::vector<u8>& code, bool bits64) {
        MemoryByteReader reader(code.data(), code.size());
        u32 count = 0;
        while (reader.remaining() > 0) {
            DecodedInstruction instruction;
            if (decode_instruction(
                    reader, bits64 ? DecodeMode::Mode64 : DecodeMode::Mode32,
                    instruction) != DecodeError::Success) {
                break;
            }
            ++count;
        }
        return count;
    }

    std::unique_ptr<GuestMemory> m_memory;
};

TEST_F(Sse4JitParityTest, LdmxcsrMasksReservedBits) {
    const std::vector<u8> code{0x0F, 0xAE, 0x10};  // LDMXCSR [EAX/RAX]
    for (bool bits64 : {false, true}) {
        SCOPED_TRACE(bits64 ? "64" : "32");
        ASSERT_TRUE(m_memory->write_u32(kSse4JitData, 0xFFFFFFFFu));
        CpuState start = initial_cpu(bits64);
        start.sse.mxcsr = 0x1F80;
        start.sse.mxcsr_mask = 0xFFFF;

        const CpuState interpreted = run_interpreter(code, bits64, start);
        const CpuState jitted = run_native_jit(code, bits64, start);

        EXPECT_EQ(interpreted.sse.mxcsr, 0xFFFFu);
        EXPECT_EQ(jitted.sse.mxcsr, interpreted.sse.mxcsr);
        EXPECT_EQ(jitted.rip, interpreted.rip);
    }
}

TEST_F(Sse4JitParityTest, StmxcsrStoresMxcsr) {
    const std::vector<u8> code{0x0F, 0xAE, 0x18};  // STMXCSR [EAX/RAX]
    for (bool bits64 : {false, true}) {
        SCOPED_TRACE(bits64 ? "64" : "32");
        ASSERT_TRUE(m_memory->write_u32(kSse4JitData, 0xDEADBEEFu));
        CpuState start = initial_cpu(bits64);
        start.sse.mxcsr = 0x1F80;

        CpuState interpreted = run_interpreter(code, bits64, start);
        auto interp_stored = m_memory->read_u32(kSse4JitData);
        ASSERT_TRUE(interp_stored);

        ASSERT_TRUE(m_memory->write_u32(kSse4JitData, 0xDEADBEEFu));
        CpuState jitted = run_native_jit(code, bits64, start);
        auto jit_stored = m_memory->read_u32(kSse4JitData);
        ASSERT_TRUE(jit_stored);

        EXPECT_EQ(interp_stored.value(), 0x1F80u);
        EXPECT_EQ(jit_stored.value(), interp_stored.value());
        EXPECT_EQ(jitted.sse.mxcsr, interpreted.sse.mxcsr);
        EXPECT_EQ(jitted.rip, interpreted.rip);
    }
}

TEST_F(Sse4JitParityTest, PblendvbRegMatchesInterpreter) {
    // PBLENDVB XMM1, XMM2
    const std::vector<u8> code{0x66, 0x0F, 0x38, 0x10, 0xCA};
    for (bool bits64 : {false, true}) {
        SCOPED_TRACE(bits64 ? "64" : "32");
        CpuState start = initial_cpu(bits64);
        seed_blend_regs(start);
        const CpuState interpreted = run_interpreter(code, bits64, start);
        const CpuState jitted = run_native_jit(code, bits64, start);
        EXPECT_EQ(jitted.sse.xmm[1].u64_[0], interpreted.sse.xmm[1].u64_[0]);
        EXPECT_EQ(jitted.sse.xmm[1].u64_[1], interpreted.sse.xmm[1].u64_[1]);
        EXPECT_EQ(jitted.sse.xmm[0].u64_[0], interpreted.sse.xmm[0].u64_[0]);
        EXPECT_EQ(jitted.rip, interpreted.rip);
    }
}

TEST_F(Sse4JitParityTest, PblendvbMemMatchesInterpreter) {
    // PBLENDVB XMM1, [EAX/RAX]
    const std::vector<u8> code{0x66, 0x0F, 0x38, 0x10, 0x08};
    const u8 src[16] = {
        0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
        0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7,
    };
    for (bool bits64 : {false, true}) {
        SCOPED_TRACE(bits64 ? "64" : "32");
        ASSERT_TRUE(m_memory->write_bytes(kSse4JitData, src, sizeof(src)));
        CpuState start = initial_cpu(bits64);
        seed_blend_regs(start);
        const CpuState interpreted = run_interpreter(code, bits64, start);
        ASSERT_TRUE(m_memory->write_bytes(kSse4JitData, src, sizeof(src)));
        const CpuState jitted = run_native_jit(code, bits64, start);
        EXPECT_EQ(jitted.sse.xmm[1].u64_[0], interpreted.sse.xmm[1].u64_[0]);
        EXPECT_EQ(jitted.sse.xmm[1].u64_[1], interpreted.sse.xmm[1].u64_[1]);
        EXPECT_EQ(jitted.rip, interpreted.rip);
    }
}

TEST_F(Sse4JitParityTest, BlendvpsRegMatchesInterpreter) {
    // BLENDVPS XMM1, XMM2
    const std::vector<u8> code{0x66, 0x0F, 0x38, 0x14, 0xCA};
    for (bool bits64 : {false, true}) {
        SCOPED_TRACE(bits64 ? "64" : "32");
        CpuState start = initial_cpu(bits64);
        seed_blend_regs(start);
        const CpuState interpreted = run_interpreter(code, bits64, start);
        const CpuState jitted = run_native_jit(code, bits64, start);
        EXPECT_EQ(jitted.sse.xmm[1].u32_[0], interpreted.sse.xmm[1].u32_[0]);
        EXPECT_EQ(jitted.sse.xmm[1].u32_[1], interpreted.sse.xmm[1].u32_[1]);
        EXPECT_EQ(jitted.sse.xmm[1].u32_[2], interpreted.sse.xmm[1].u32_[2]);
        EXPECT_EQ(jitted.sse.xmm[1].u32_[3], interpreted.sse.xmm[1].u32_[3]);
        EXPECT_EQ(jitted.rip, interpreted.rip);
    }
}

TEST_F(Sse4JitParityTest, BlendvpdRegMatchesInterpreter) {
    // BLENDVPD XMM1, XMM2
    const std::vector<u8> code{0x66, 0x0F, 0x38, 0x15, 0xCA};
    for (bool bits64 : {false, true}) {
        SCOPED_TRACE(bits64 ? "64" : "32");
        CpuState start = initial_cpu(bits64);
        seed_blend_regs(start);
        const CpuState interpreted = run_interpreter(code, bits64, start);
        const CpuState jitted = run_native_jit(code, bits64, start);
        EXPECT_EQ(jitted.sse.xmm[1].u64_[0], interpreted.sse.xmm[1].u64_[0]);
        EXPECT_EQ(jitted.sse.xmm[1].u64_[1], interpreted.sse.xmm[1].u64_[1]);
        EXPECT_EQ(jitted.rip, interpreted.rip);
    }
}

namespace {

constexpr u8 kEcx = static_cast<u8>(RegIndex::RCX);
constexpr u8 kEdx = static_cast<u8>(RegIndex::RDX);
constexpr u8 kEbx = static_cast<u8>(RegIndex::RBX);

void seed_pcmp_hello(CpuState& cpu) {
    const char str[] = "Hello";
    std::memset(&cpu.sse.xmm[0], 0, 16);
    std::memset(&cpu.sse.xmm[1], 0, 16);
    std::memcpy(&cpu.sse.xmm[0], str, sizeof(str));
    std::memcpy(&cpu.sse.xmm[1], str, sizeof(str));
}

void expect_pcmp_parity(const CpuState& jitted, const CpuState& interpreted) {
    EXPECT_EQ(jitted.gpr.get32(kEcx), interpreted.gpr.get32(kEcx));
    EXPECT_EQ(jitted.gpr.get32(kEax), interpreted.gpr.get32(kEax));
    EXPECT_EQ(jitted.gpr.get32(kEdx), interpreted.gpr.get32(kEdx));
    EXPECT_EQ(jitted.gpr.get32(kEbx), interpreted.gpr.get32(kEbx));
    EXPECT_EQ(jitted.sse.xmm[0].u64_[0], interpreted.sse.xmm[0].u64_[0]);
    EXPECT_EQ(jitted.sse.xmm[0].u64_[1], interpreted.sse.xmm[0].u64_[1]);
    EXPECT_EQ(jitted.flags.cf(), interpreted.flags.cf());
    EXPECT_EQ(jitted.flags.zf(), interpreted.flags.zf());
    EXPECT_EQ(jitted.flags.sf(), interpreted.flags.sf());
    EXPECT_EQ(jitted.flags.of(), interpreted.flags.of());
    EXPECT_EQ(jitted.rip, interpreted.rip);
}

}  // namespace

TEST_F(Sse4JitParityTest, PcmpistriRegMatchesInterpreter) {
    // PCMPISTRI XMM0, XMM1, 0x08  (byte, equal-each, LSB index)
    const std::vector<u8> code{0x66, 0x0F, 0x3A, 0x63, 0xC1, 0x08};
    for (bool bits64 : {false, true}) {
        SCOPED_TRACE(bits64 ? "64" : "32");
        CpuState start = initial_cpu(bits64);
        seed_pcmp_hello(start);
        const CpuState interpreted = run_interpreter(code, bits64, start);
        const CpuState jitted = run_native_jit(code, bits64, start);
        EXPECT_EQ(interpreted.gpr.get32(kEcx), 0u);
        expect_pcmp_parity(jitted, interpreted);
    }
}

TEST_F(Sse4JitParityTest, PcmpistriMemMatchesInterpreter) {
    // PCMPISTRI XMM0, [EAX/RAX], 0x08
    const std::vector<u8> code{0x66, 0x0F, 0x3A, 0x63, 0x00, 0x08};
    u8 src[16]{};
    std::memcpy(src, "Hello", 6);
    for (bool bits64 : {false, true}) {
        SCOPED_TRACE(bits64 ? "64" : "32");
        ASSERT_TRUE(m_memory->write_bytes(kSse4JitData, src, sizeof(src)));
        CpuState start = initial_cpu(bits64);
        std::memset(&start.sse.xmm[0], 0, 16);
        std::memcpy(&start.sse.xmm[0], "Hello", 6);
        const CpuState interpreted = run_interpreter(code, bits64, start);
        ASSERT_TRUE(m_memory->write_bytes(kSse4JitData, src, sizeof(src)));
        const CpuState jitted = run_native_jit(code, bits64, start);
        EXPECT_EQ(interpreted.gpr.get32(kEcx), 0u);
        expect_pcmp_parity(jitted, interpreted);
    }
}

TEST_F(Sse4JitParityTest, PcmpestriExplicitLengthPreservesEaxEdx) {
    // PCMPESTRI XMM0, XMM1, 0x48: equal-each, MSB index. EAX=EDX=3 so only
    // the first three bytes are valid; both-invalid lanes still set IntRes2
    // and the MSB index is 15. RAX/RDX (including high halves) must survive.
    const std::vector<u8> code{0x66, 0x0F, 0x3A, 0x61, 0xC1, 0x48};
    for (bool bits64 : {false, true}) {
        SCOPED_TRACE(bits64 ? "64" : "32");
        CpuState start = initial_cpu(bits64);
        std::memset(&start.sse.xmm[0], 'X', 16);
        std::memset(&start.sse.xmm[1], 'Y', 16);
        std::memcpy(&start.sse.xmm[0], "AAA", 3);
        std::memcpy(&start.sse.xmm[1], "AAA", 3);
        if (bits64) {
            start.gpr.set64(kEax, 0x1111111100000003ULL);
            start.gpr.set64(kEdx, 0x2222222200000003ULL);
        } else {
            start.gpr.set32(kEax, 3);
            start.gpr.set32(kEdx, 3);
        }
        const CpuState interpreted = run_interpreter(code, bits64, start);
        const CpuState jitted = run_native_jit(code, bits64, start);
        EXPECT_EQ(interpreted.gpr.get32(kEcx), 15u);
        EXPECT_TRUE(interpreted.flags.sf());
        EXPECT_TRUE(interpreted.flags.zf());
        expect_pcmp_parity(jitted, interpreted);
        if (bits64) {
            EXPECT_EQ(jitted.gpr.get64(kEax), start.gpr.get64(kEax));
            EXPECT_EQ(jitted.gpr.get64(kEdx), start.gpr.get64(kEdx));
        }
    }
}

TEST_F(Sse4JitParityTest, PcmpestriMemUsesEaxLengthNotEa) {
    // PCMPESTRI XMM0, [EBX/RBX], 0x48. EAX/EDX=3, EA in RBX. Parking the
    // address in RAX would saturate length to 16, clear SF, and write ECX=2
    // (MSB of only the three equal valid bytes).
    const std::vector<u8> code{0x66, 0x0F, 0x3A, 0x61, 0x03, 0x48};
    u8 src[16];
    std::memset(src, 'Y', sizeof(src));
    std::memcpy(src, "AAA", 3);
    for (bool bits64 : {false, true}) {
        SCOPED_TRACE(bits64 ? "64" : "32");
        ASSERT_TRUE(m_memory->write_bytes(kSse4JitData, src, sizeof(src)));
        CpuState start = initial_cpu(bits64);
        std::memset(&start.sse.xmm[0], 'X', 16);
        std::memcpy(&start.sse.xmm[0], "AAA", 3);
        if (bits64) {
            start.gpr.set64(kEax, 3);
            start.gpr.set64(kEdx, 3);
            start.gpr.set64(kEbx, kSse4JitData);
        } else {
            start.gpr.set32(kEax, 3);
            start.gpr.set32(kEdx, 3);
            start.gpr.set32(kEbx, static_cast<u32>(kSse4JitData));
        }
        const CpuState interpreted = run_interpreter(code, bits64, start);
        ASSERT_TRUE(m_memory->write_bytes(kSse4JitData, src, sizeof(src)));
        const CpuState jitted = run_native_jit(code, bits64, start);
        EXPECT_EQ(interpreted.gpr.get32(kEcx), 15u);
        EXPECT_TRUE(interpreted.flags.sf());
        expect_pcmp_parity(jitted, interpreted);
        EXPECT_EQ(jitted.gpr.get32(kEbx), interpreted.gpr.get32(kEbx));
        EXPECT_EQ(jitted.gpr.get32(kEax), 3u);
    }
}

TEST_F(Sse4JitParityTest, PcmpistrmRegMatchesInterpreter) {
    // PCMPISTRM XMM1, XMM2, 0x08 — mask is written to XMM0, not dest.
    const std::vector<u8> code{0x66, 0x0F, 0x3A, 0x62, 0xCA, 0x08};
    for (bool bits64 : {false, true}) {
        SCOPED_TRACE(bits64 ? "64" : "32");
        CpuState start = initial_cpu(bits64);
        const char str[] = "Hello";
        std::memset(&start.sse.xmm[0], 0, 16);
        std::memset(&start.sse.xmm[1], 0, 16);
        std::memset(&start.sse.xmm[2], 0, 16);
        std::memcpy(&start.sse.xmm[1], str, sizeof(str));
        std::memcpy(&start.sse.xmm[2], str, sizeof(str));
        const CpuState interpreted = run_interpreter(code, bits64, start);
        const CpuState jitted = run_native_jit(code, bits64, start);
        expect_pcmp_parity(jitted, interpreted);
        EXPECT_EQ(jitted.sse.xmm[1].u64_[0], interpreted.sse.xmm[1].u64_[0]);
    }
}

TEST_F(Sse4JitParityTest, PhminposuwRegMatchesInterpreter) {
    // PHMINPOSUW XMM0, XMM1 — min unsigned word is lane 4, not lane 0.
    const std::vector<u8> code{0x66, 0x0F, 0x38, 0x41, 0xC1};
    for (bool bits64 : {false, true}) {
        SCOPED_TRACE(bits64 ? "64" : "32");
        CpuState start = initial_cpu(bits64);
        start.sse.xmm[0].u64_[0] = 0xFFFFFFFFFFFFFFFFULL;
        start.sse.xmm[0].u64_[1] = 0xFFFFFFFFFFFFFFFFULL;
        start.sse.xmm[1].u16_[0] = 0xFFFF;
        start.sse.xmm[1].u16_[1] = 0x8000;
        start.sse.xmm[1].u16_[2] = 0x1234;
        start.sse.xmm[1].u16_[3] = 0x00FF;
        start.sse.xmm[1].u16_[4] = 0x0007;
        start.sse.xmm[1].u16_[5] = 0xABCD;
        start.sse.xmm[1].u16_[6] = 0x7FFF;
        start.sse.xmm[1].u16_[7] = 0x0010;

        const CpuState interpreted = run_interpreter(code, bits64, start);
        const CpuState jitted = run_native_jit(code, bits64, start);

        EXPECT_EQ(interpreted.sse.xmm[0].u16_[0], 0x0007u);
        EXPECT_EQ(interpreted.sse.xmm[0].u16_[1], 4u);
        EXPECT_EQ(interpreted.sse.xmm[0].u64_[0], 0x0000000000040007ULL);
        EXPECT_EQ(interpreted.sse.xmm[0].u64_[1], 0u);
        EXPECT_EQ(jitted.sse.xmm[0].u64_[0], interpreted.sse.xmm[0].u64_[0]);
        EXPECT_EQ(jitted.sse.xmm[0].u64_[1], interpreted.sse.xmm[0].u64_[1]);
        EXPECT_EQ(jitted.rip, interpreted.rip);
    }
}

TEST_F(Sse4JitParityTest, MovntdqaMemMatchesInterpreter) {
    // MOVNTDQA XMM1, [EAX/RAX]
    const std::vector<u8> code{0x66, 0x0F, 0x38, 0x2A, 0x08};
    const u8 src[16] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    };
    for (bool bits64 : {false, true}) {
        SCOPED_TRACE(bits64 ? "64" : "32");
        ASSERT_TRUE(m_memory->write_bytes(kSse4JitData, src, sizeof(src)));
        CpuState start = initial_cpu(bits64);
        start.sse.xmm[1].u64_[0] = 0xDEADBEEFDEADBEEFULL;
        start.sse.xmm[1].u64_[1] = 0xCAFEBABECAFEBABEULL;

        const CpuState interpreted = run_interpreter(code, bits64, start);
        ASSERT_TRUE(m_memory->write_bytes(kSse4JitData, src, sizeof(src)));
        const CpuState jitted = run_native_jit(code, bits64, start);

        EXPECT_EQ(interpreted.sse.xmm[1].u64_[0], 0x0706050403020100ULL);
        EXPECT_EQ(interpreted.sse.xmm[1].u64_[1], 0x0F0E0D0C0B0A0908ULL);
        EXPECT_EQ(jitted.sse.xmm[1].u64_[0], interpreted.sse.xmm[1].u64_[0]);
        EXPECT_EQ(jitted.sse.xmm[1].u64_[1], interpreted.sse.xmm[1].u64_[1]);
        EXPECT_EQ(jitted.rip, interpreted.rip);
    }
}

TEST_F(Sse4JitParityTest, DppsRegMatchesInterpreter) {
    // DPPS XMM0, XMM1, 0xF1 — all products, broadcast to lane 0 only.
    const std::vector<u8> code{0x66, 0x0F, 0x3A, 0x40, 0xC1, 0xF1};
    const float a[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    const float b[4] = {5.0f, 6.0f, 7.0f, 8.0f};
    for (bool bits64 : {false, true}) {
        SCOPED_TRACE(bits64 ? "64" : "32");
        CpuState start = initial_cpu(bits64);
        std::memcpy(start.sse.xmm[0].u32_, a, sizeof(a));
        std::memcpy(start.sse.xmm[1].u32_, b, sizeof(b));

        const CpuState interpreted = run_interpreter(code, bits64, start);
        const CpuState jitted = run_native_jit(code, bits64, start);

        float sum = 0.0f;
        std::memcpy(&sum, &interpreted.sse.xmm[0].u32_[0], sizeof(sum));
        EXPECT_FLOAT_EQ(sum, 70.0f);
        EXPECT_EQ(interpreted.sse.xmm[0].u32_[1], 0u);
        EXPECT_EQ(interpreted.sse.xmm[0].u32_[2], 0u);
        EXPECT_EQ(interpreted.sse.xmm[0].u32_[3], 0u);
        EXPECT_EQ(jitted.sse.xmm[0].u32_[0], interpreted.sse.xmm[0].u32_[0]);
        EXPECT_EQ(jitted.sse.xmm[0].u32_[1], interpreted.sse.xmm[0].u32_[1]);
        EXPECT_EQ(jitted.sse.xmm[0].u32_[2], interpreted.sse.xmm[0].u32_[2]);
        EXPECT_EQ(jitted.sse.xmm[0].u32_[3], interpreted.sse.xmm[0].u32_[3]);
        EXPECT_EQ(jitted.rip, interpreted.rip);
    }
}

TEST_F(Sse4JitParityTest, DppdRegMatchesInterpreter) {
    // DPPD XMM0, XMM1, 0x31 — both products, broadcast to low qword only.
    const std::vector<u8> code{0x66, 0x0F, 0x3A, 0x41, 0xC1, 0x31};
    const double a[2] = {1.0, 2.0};
    const double b[2] = {3.0, 4.0};
    for (bool bits64 : {false, true}) {
        SCOPED_TRACE(bits64 ? "64" : "32");
        CpuState start = initial_cpu(bits64);
        std::memcpy(start.sse.xmm[0].u64_, a, sizeof(a));
        std::memcpy(start.sse.xmm[1].u64_, b, sizeof(b));

        const CpuState interpreted = run_interpreter(code, bits64, start);
        const CpuState jitted = run_native_jit(code, bits64, start);

        double sum = 0.0;
        std::memcpy(&sum, &interpreted.sse.xmm[0].u64_[0], sizeof(sum));
        EXPECT_DOUBLE_EQ(sum, 11.0);
        EXPECT_EQ(interpreted.sse.xmm[0].u64_[1], 0u);
        EXPECT_EQ(jitted.sse.xmm[0].u64_[0], interpreted.sse.xmm[0].u64_[0]);
        EXPECT_EQ(jitted.sse.xmm[0].u64_[1], interpreted.sse.xmm[0].u64_[1]);
        EXPECT_EQ(jitted.rip, interpreted.rip);
    }
}

TEST_F(Sse4JitParityTest, MpsadbwRegMatchesInterpreter) {
    // MPSADBW XMM0, XMM1, 0 — src/dst offsets 0, eight 4-byte SADs.
    const std::vector<u8> code{0x66, 0x0F, 0x3A, 0x42, 0xC1, 0x00};
    for (bool bits64 : {false, true}) {
        SCOPED_TRACE(bits64 ? "64" : "32");
        CpuState start = initial_cpu(bits64);
        for (int i = 0; i < 16; ++i) {
            start.sse.xmm[0].u8_[i] = static_cast<u8>(i * 3);
            start.sse.xmm[1].u8_[i] = static_cast<u8>(i + 10);
        }

        const CpuState interpreted = run_interpreter(code, bits64, start);
        const CpuState jitted = run_native_jit(code, bits64, start);

        EXPECT_EQ(jitted.sse.xmm[0].u64_[0], interpreted.sse.xmm[0].u64_[0]);
        EXPECT_EQ(jitted.sse.xmm[0].u64_[1], interpreted.sse.xmm[0].u64_[1]);
        EXPECT_EQ(jitted.rip, interpreted.rip);
    }
}

TEST_F(Sse4JitParityTest, BlendvpsMemMatchesInterpreter) {
    // BLENDVPS XMM1, [EAX/RAX]
    const std::vector<u8> code{0x66, 0x0F, 0x38, 0x14, 0x08};
    const u8 src[16] = {
        0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
        0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7,
    };
    for (bool bits64 : {false, true}) {
        SCOPED_TRACE(bits64 ? "64" : "32");
        ASSERT_TRUE(m_memory->write_bytes(kSse4JitData, src, sizeof(src)));
        CpuState start = initial_cpu(bits64);
        seed_blend_regs(start);
        const CpuState interpreted = run_interpreter(code, bits64, start);
        ASSERT_TRUE(m_memory->write_bytes(kSse4JitData, src, sizeof(src)));
        const CpuState jitted = run_native_jit(code, bits64, start);
        EXPECT_EQ(jitted.sse.xmm[1].u32_[0], interpreted.sse.xmm[1].u32_[0]);
        EXPECT_EQ(jitted.sse.xmm[1].u32_[1], interpreted.sse.xmm[1].u32_[1]);
        EXPECT_EQ(jitted.sse.xmm[1].u32_[2], interpreted.sse.xmm[1].u32_[2]);
        EXPECT_EQ(jitted.sse.xmm[1].u32_[3], interpreted.sse.xmm[1].u32_[3]);
        EXPECT_EQ(jitted.rip, interpreted.rip);
    }
}

TEST_F(Sse4JitParityTest, BlendvpdMemMatchesInterpreter) {
    // BLENDVPD XMM1, [EAX/RAX]
    const std::vector<u8> code{0x66, 0x0F, 0x38, 0x15, 0x08};
    const u8 src[16] = {
        0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
        0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7,
    };
    for (bool bits64 : {false, true}) {
        SCOPED_TRACE(bits64 ? "64" : "32");
        ASSERT_TRUE(m_memory->write_bytes(kSse4JitData, src, sizeof(src)));
        CpuState start = initial_cpu(bits64);
        seed_blend_regs(start);
        const CpuState interpreted = run_interpreter(code, bits64, start);
        ASSERT_TRUE(m_memory->write_bytes(kSse4JitData, src, sizeof(src)));
        const CpuState jitted = run_native_jit(code, bits64, start);
        EXPECT_EQ(jitted.sse.xmm[1].u64_[0], interpreted.sse.xmm[1].u64_[0]);
        EXPECT_EQ(jitted.sse.xmm[1].u64_[1], interpreted.sse.xmm[1].u64_[1]);
        EXPECT_EQ(jitted.rip, interpreted.rip);
    }
}
