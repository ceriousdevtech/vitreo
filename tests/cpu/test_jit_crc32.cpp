// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// SSE4.2 CRC32C interpreter/JIT contract tests

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/jit/stencils.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

namespace {

constexpr u8 EAX = static_cast<u8>(RegIndex::RAX);
constexpr u8 ECX = static_cast<u8>(RegIndex::RCX);
constexpr u8 EDX = static_cast<u8>(RegIndex::RDX);
constexpr u8 ESP = static_cast<u8>(RegIndex::RSP);

constexpr GuestAddr kCode = 0x1000;
constexpr GuestAddr kData = 0x3000;
constexpr u32 kInitialCrc = 0x89ABCDEFu;
constexpr u32 kSource = 0x10203040u;
constexpr u64 kInitialFlags = 0x8D7u;

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

u32 crc32c_reference(u32 crc, u32 data, unsigned byte_count) {
    constexpr u32 reflected_castagnoli = 0x82F63B78u;
    for (unsigned byte = 0; byte < byte_count; ++byte) {
        crc ^= data & 0xFFu;
        data >>= 8;
        for (unsigned bit = 0; bit < 8; ++bit) {
            const u32 mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (reflected_castagnoli & mask);
        }
    }
    return crc;
}

struct CrcForm {
    const char* name;
    std::vector<u8> register_code;
    std::vector<u8> memory_code;
    unsigned byte_count;
};

const std::array<CrcForm, 3> kForms{{
    {"r/m8",
     {0xF2, 0x0F, 0x38, 0xF0, 0xC1},
     {0xF2, 0x0F, 0x38, 0xF0, 0x02},
     1},
    {"r/m16",
     {0x66, 0xF2, 0x0F, 0x38, 0xF1, 0xC1},
     {0x66, 0xF2, 0x0F, 0x38, 0xF1, 0x02},
     2},
    {"r/m32",
     {0xF2, 0x0F, 0x38, 0xF1, 0xC1},
     {0xF2, 0x0F, 0x38, 0xF1, 0x02},
     4},
}};

class JitCrc32Test : public ::testing::Test {
protected:
    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        ASSERT_TRUE(m_memory->map_region(kCode, 0x10000,
                                         MemoryProtection::ReadWrite));
        ASSERT_TRUE(m_memory->write_u32(kData, kSource));
    }

    CpuState initial_cpu() const {
        CpuState cpu;
        cpu.init_protected_mode();
        cpu.gpr.set32(EAX, kInitialCrc);
        cpu.gpr.set32(ECX, kSource);
        cpu.gpr.set32(EDX, kData);
        cpu.gpr.set32(ESP, 0x10000);
        cpu.flags.rflags = kInitialFlags;
        cpu.rip = kCode;
        return cpu;
    }

    BasicBlock decode_block(const std::vector<u8>& code) {
        BasicBlock block;
        block.start_addr = kCode;

        MemoryByteReader reader(code.data(), code.size());
        while (reader.remaining() > 0) {
            DecodedInstruction instruction;
            const DecodeError error =
                decode_instruction(reader, DecodeMode::Mode32, instruction);
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

    CpuState run_interpreter(const std::vector<u8>& code) {
        EXPECT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
        CpuState cpu = initial_cpu();
        Interpreter interpreter(cpu, *m_memory);
        EXPECT_EQ(interpreter.run(1), ExecResult::Continue);
        return cpu;
    }

    CpuState run_native_jit(const std::vector<u8>& code) {
        EXPECT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
        BasicBlock block = decode_block(code);
        CpuState cpu = initial_cpu();

        ScopedEnvironmentVariable no_delegate(
            "VITREO_JIT_NO_DELEGATE", "1");
        JitCompiler jit(*m_memory);
        CompiledBlock* compiled = jit.compile(block);
        EXPECT_NE(compiled, nullptr)
            << "CRC32 must compile while delegation is disabled";
        if (compiled) {
            EXPECT_EQ(jit.execute(*compiled, cpu), JitExecResult::Continue);
        }
        return cpu;
    }

    void expect_contract(const CpuState& cpu, unsigned byte_count,
                         usize instruction_length) {
        EXPECT_EQ(cpu.gpr.get32(EAX),
                  crc32c_reference(kInitialCrc, kSource, byte_count));
        EXPECT_EQ(cpu.flags.rflags, kInitialFlags);
        EXPECT_EQ(cpu.rip, kCode + instruction_length);
    }

    std::unique_ptr<GuestMemory> m_memory;
};

TEST(Arm64Crc32EncodingTest, MatchesAssemblerEncodings) {
    EXPECT_EQ(arm64::crc32cb_w(0, 1, 2), 0x1AC25020u);
    EXPECT_EQ(arm64::crc32ch_w(3, 4, 5), 0x1AC55483u);
    EXPECT_EQ(arm64::crc32cw_w(6, 7, 8), 0x1AC858E6u);
    EXPECT_EQ(arm64::crc32cx_w(6, 7, 8), 0x9AC85CE6u);
}

TEST_F(JitCrc32Test, RegisterFormsMatchReferenceWithoutChangingFlags) {
    for (const CrcForm& form : kForms) {
        SCOPED_TRACE(form.name);
        const CpuState interpreted = run_interpreter(form.register_code);
        const CpuState jitted = run_native_jit(form.register_code);
        expect_contract(interpreted, form.byte_count,
                        form.register_code.size());
        expect_contract(jitted, form.byte_count, form.register_code.size());
        EXPECT_EQ(jitted.gpr.get32(EAX), interpreted.gpr.get32(EAX));
    }
}

TEST_F(JitCrc32Test, LegacyHighByteSourceUsesOriginalAh) {
    const std::vector<u8> code{
        0xF2, 0x0F, 0x38, 0xF0, 0xC4}; // CRC32 EAX, AH
    const u32 source = (kInitialCrc >> 8) & 0xFFu;
    const u32 expected = crc32c_reference(kInitialCrc, source, 1);

    const CpuState interpreted = run_interpreter(code);
    const CpuState jitted = run_native_jit(code);

    EXPECT_EQ(interpreted.gpr.get32(EAX), expected);
    EXPECT_EQ(jitted.gpr.get32(EAX), expected);
    EXPECT_EQ(interpreted.flags.rflags, kInitialFlags);
    EXPECT_EQ(jitted.flags.rflags, kInitialFlags);
}

TEST_F(JitCrc32Test, MemoryFormsMatchReferenceWithoutChangingFlags) {
    for (const CrcForm& form : kForms) {
        SCOPED_TRACE(form.name);
        const CpuState interpreted = run_interpreter(form.memory_code);
        const CpuState jitted = run_native_jit(form.memory_code);
        expect_contract(interpreted, form.byte_count, form.memory_code.size());
        expect_contract(jitted, form.byte_count, form.memory_code.size());
        EXPECT_EQ(jitted.gpr.get32(EAX), interpreted.gpr.get32(EAX));
    }
}

TEST_F(JitCrc32Test, RexWRm8ZeroExtendsIntoR64) {
    const std::vector<u8> code{
        0xF2, 0x48, 0x0F, 0x38, 0xF0, 0xC1}; // CRC32 RAX, CL
    ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));

    BasicBlock block;
    block.start_addr = kCode;
    block.is_64bit = true;
    MemoryByteReader reader(code.data(), code.size());
    DecodedInstruction instruction;
    ASSERT_EQ(
        decode_instruction(reader, DecodeMode::Mode64, instruction),
        DecodeError::Success);
    ASSERT_EQ(instruction.opcode, Opcode::CRC32_R64_RM8);
    ASSERT_EQ(instruction.operand_count, 2);
    EXPECT_EQ(instruction.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(instruction.operands[1].type, OperandType::Reg8);
    block.instructions.push_back(instruction);
    block.byte_length = static_cast<u32>(code.size());
    block.instruction_count = 1;

    const u32 expected = crc32c_reference(kInitialCrc, kSource, 1);
    auto initial = [&]() {
        CpuState cpu;
        cpu.init_long_mode();
        cpu.gpr.set64(EAX, 0xFFFFFFFF00000000ULL | kInitialCrc);
        cpu.gpr.set64(ECX, kSource);
        cpu.gpr.set64(ESP, 0x10000);
        cpu.flags.rflags = kInitialFlags;
        cpu.rip = kCode;
        return cpu;
    };

    CpuState interpreted = initial();
    Interpreter interpreter(interpreted, *m_memory);
    EXPECT_EQ(interpreter.run(1), ExecResult::Continue);

    CpuState jitted = initial();
    ScopedEnvironmentVariable no_delegate(
        "VITREO_JIT_NO_DELEGATE", "1");
    JitCompiler jit(*m_memory);
    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr);
    EXPECT_EQ(jit.execute(*compiled, jitted), JitExecResult::Continue);

    EXPECT_EQ(interpreted.gpr.get64(EAX), expected);
    EXPECT_EQ(jitted.gpr.get64(EAX), expected);
    EXPECT_EQ(interpreted.flags.rflags, kInitialFlags);
    EXPECT_EQ(jitted.flags.rflags, kInitialFlags);
}

TEST_F(JitCrc32Test, RexWProcessesAllEightSourceBytes) {
    constexpr u64 source = 0x8877665544332211ULL;
    const std::vector<u8> code{
        0xF2, 0x48, 0x0F, 0x38, 0xF1, 0x02}; // CRC32 RAX, [RDX]
    ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
    ASSERT_TRUE(m_memory->write_u64(kData, source));

    BasicBlock block;
    block.start_addr = kCode;
    block.is_64bit = true;
    MemoryByteReader reader(code.data(), code.size());
    DecodedInstruction instruction;
    ASSERT_EQ(
        decode_instruction(reader, DecodeMode::Mode64, instruction),
        DecodeError::Success);
    ASSERT_EQ(instruction.opcode, Opcode::CRC32_R64_RM64);
    block.instructions.push_back(instruction);
    block.byte_length = static_cast<u32>(code.size());
    block.instruction_count = 1;

    const u32 after_low = crc32c_reference(
        kInitialCrc, static_cast<u32>(source), 4);
    const u32 expected = crc32c_reference(
        after_low, static_cast<u32>(source >> 32), 4);
    auto initial = [&]() {
        CpuState cpu;
        cpu.init_long_mode();
        cpu.gpr.set64(EAX, 0xFFFFFFFF00000000ULL | kInitialCrc);
        cpu.gpr.set64(EDX, kData);
        cpu.gpr.set64(ESP, 0x10000);
        cpu.flags.rflags = kInitialFlags;
        cpu.rip = kCode;
        return cpu;
    };

    CpuState interpreted = initial();
    Interpreter interpreter(interpreted, *m_memory);
    EXPECT_EQ(interpreter.run(1), ExecResult::Continue);

    CpuState jitted = initial();
    ScopedEnvironmentVariable no_delegate(
        "VITREO_JIT_NO_DELEGATE", "1");
    JitCompiler jit(*m_memory);
    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr);
    EXPECT_EQ(jit.execute(*compiled, jitted), JitExecResult::Continue);

    EXPECT_EQ(interpreted.gpr.get64(EAX), expected);
    EXPECT_EQ(jitted.gpr.get64(EAX), expected);
    EXPECT_EQ(interpreted.flags.rflags, kInitialFlags);
    EXPECT_EQ(jitted.flags.rflags, kInitialFlags);
}

TEST_F(JitCrc32Test, MemoryFaultDoesNotModifyDestination) {
    const std::vector<u8> code{
        0xBB, 0x01, 0x00, 0x00, 0x00, // MOV EBX, 1
        0xF2, 0x0F, 0x38, 0xF1, 0x02  // CRC32 EAX, [EDX]
    };
    ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
    BasicBlock block = decode_block(code);
    CpuState cpu = initial_cpu();
    cpu.gpr.set32(EDX, 0x20000000u);

    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    JitCompiler jit(*m_memory);
    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr);
    EXPECT_EQ(jit.execute(*compiled, cpu), JitExecResult::Continue);

    ASSERT_TRUE(cpu.pending_raise.active);
    EXPECT_TRUE(cpu.pending_raise.from_helper);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    EXPECT_EQ(cpu.gpr.get32(EAX), kInitialCrc);
    EXPECT_EQ(cpu.flags.rflags, kInitialFlags);
    EXPECT_EQ(cpu.rip, kCode + 5);
}

} // namespace
