// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Direct NEON SSE2 integer JIT contract tests

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
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

constexpr GuestAddr kCode = 0x1000;
constexpr GuestAddr kData = 0x3000;
constexpr u8 EDX = static_cast<u8>(RegIndex::RDX);
constexpr u8 ESP = static_cast<u8>(RegIndex::RSP);
constexpr u64 kInitialFlags = 0x8D7u;
constexpr u32 kBlrX16 = 0xD63F0200u;

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

struct Sse2Form {
    const char* name;
    u8 opcode;
};

constexpr std::array<Sse2Form, 13> kRegisterForms{{
    {"PMULLW", 0xD5},
    {"PMULHW", 0xE5},
    {"PMULHUW", 0xE4},
    {"PMINUB", 0xDA},
    {"PMAXUB", 0xDE},
    {"PMINSW", 0xEA},
    {"PMAXSW", 0xEE},
    {"PAVGB", 0xE0},
    {"PAVGW", 0xE3},
    {"PADDUSB", 0xDC},
    {"PADDUSW", 0xDD},
    {"PSUBUSB", 0xD8},
    {"PSUBUSW", 0xD9},
}};

class JitSse2NativeTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        ASSERT_TRUE(m_memory->map_region(
            kCode, 0x10000, MemoryProtection::ReadWrite));
    }

    CpuState initial_cpu() const {
        CpuState cpu;
        cpu.init_protected_mode();
        cpu.gpr.set32(EDX, kData);
        cpu.gpr.set32(ESP, 0x10000);
        cpu.flags.rflags = kInitialFlags;
        cpu.rip = kCode;

        const std::array<u16, 8> dst{{
            0x0000, 0x0001, 0x007F, 0x0080,
            0x7FFF, 0x8000, 0xFF00, 0xFFFF,
        }};
        const std::array<u16, 8> src{{
            0xFFFF, 0x0002, 0x0080, 0x007F,
            0x0002, 0x8001, 0x0200, 0x0001,
        }};
        std::memcpy(cpu.sse.xmm[0].u16_, dst.data(), sizeof(dst));
        std::memcpy(cpu.sse.xmm[1].u16_, src.data(), sizeof(src));
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

    CpuState run_jit(const std::vector<u8>& code,
                     bool expect_sse_helper_call) {
        EXPECT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
        BasicBlock block = decode_block(code);
        CpuState cpu = initial_cpu();

        ScopedEnvironmentVariable no_delegate(
            "VITREO_JIT_NO_DELEGATE", "1");
        JitCompiler jit(*m_memory);
        CompiledBlock* compiled = jit.compile(block);
        EXPECT_NE(compiled, nullptr);
        if (!compiled) {
            return cpu;
        }

        bool has_blr_x16 = false;
        const auto* code_bytes =
            static_cast<const u8*>(compiled->code);
        for (u32 offset = 0;
             offset + sizeof(u32) <= compiled->code_size;
             offset += sizeof(u32)) {
            u32 word = 0;
            std::memcpy(&word, code_bytes + offset, sizeof(word));
            has_blr_x16 |= (word == kBlrX16);
        }
        EXPECT_EQ(has_blr_x16, expect_sse_helper_call);
        EXPECT_EQ(jit.execute(*compiled, cpu), JitExecResult::Continue);
        return cpu;
    }

    void expect_same_result(const CpuState& interpreted,
                            const CpuState& jitted,
                            usize instruction_length) {
        EXPECT_EQ(jitted.sse.xmm[0].u64_[0],
                  interpreted.sse.xmm[0].u64_[0]);
        EXPECT_EQ(jitted.sse.xmm[0].u64_[1],
                  interpreted.sse.xmm[0].u64_[1]);
        EXPECT_EQ(jitted.flags.rflags, kInitialFlags);
        EXPECT_EQ(interpreted.flags.rflags, kInitialFlags);
        EXPECT_EQ(jitted.rip, kCode + instruction_length);
        EXPECT_EQ(interpreted.rip, kCode + instruction_length);
    }

    std::unique_ptr<GuestMemory> m_memory;
};

TEST(Arm64Sse2IntegerEncodingTest, MatchesAssemblerEncodings) {
    EXPECT_EQ(arm64::mul_v(3, 4, 5, arm64::VS_H), 0x4E659C83u);
    EXPECT_EQ(arm64::umin_v(3, 4, 5, arm64::VS_B), 0x6E256C83u);
    EXPECT_EQ(arm64::umax_v(3, 4, 5, arm64::VS_B), 0x6E256483u);
    EXPECT_EQ(arm64::smin_v(3, 4, 5, arm64::VS_H), 0x4E656C83u);
    EXPECT_EQ(arm64::smax_v(3, 4, 5, arm64::VS_H), 0x4E656483u);
    EXPECT_EQ(arm64::urhadd_v(3, 4, 5, arm64::VS_B), 0x6E251483u);
    EXPECT_EQ(arm64::urhadd_v(3, 4, 5, arm64::VS_H), 0x6E651483u);
    EXPECT_EQ(arm64::uqadd_v(3, 4, 5, arm64::VS_B), 0x6E250C83u);
    EXPECT_EQ(arm64::uqadd_v(3, 4, 5, arm64::VS_H), 0x6E650C83u);
    EXPECT_EQ(arm64::uqsub_v(3, 4, 5, arm64::VS_B), 0x6E252C83u);
    EXPECT_EQ(arm64::uqsub_v(3, 4, 5, arm64::VS_H), 0x6E652C83u);
    EXPECT_EQ(arm64::smull2_v(3, 4, 5, arm64::VS_H), 0x4E65C083u);
    EXPECT_EQ(arm64::umull2_v(3, 4, 5, arm64::VS_H), 0x6E65C083u);
}

TEST_F(JitSse2NativeTest, RegisterFormsMatchInterpreterWithoutHelperCalls) {
    for (const Sse2Form& form : kRegisterForms) {
        SCOPED_TRACE(form.name);
        const std::vector<u8> code{0x66, 0x0F, form.opcode, 0xC1};
        const CpuState interpreted = run_interpreter(code);
        const CpuState jitted = run_jit(code, false);
        expect_same_result(interpreted, jitted, code.size());
    }
}

TEST_F(JitSse2NativeTest, MovssRegPreservesHighDwordsWithoutHelper) {
    const std::vector<u8> code{0xF3, 0x0F, 0x10, 0xC1}; // MOVSS XMM0, XMM1
    const CpuState interpreted = run_interpreter(code);
    const CpuState jitted = run_jit(code, false);
    expect_same_result(interpreted, jitted, code.size());
}

TEST_F(JitSse2NativeTest, PsrldqImmMatchesInterpreterWithoutHelper) {
    const std::vector<u8> code{0x66, 0x0F, 0x73, 0xD8, 0x04}; // PSRLDQ XMM0, 4
    const CpuState interpreted = run_interpreter(code);
    const CpuState jitted = run_jit(code, false);
    expect_same_result(interpreted, jitted, code.size());
}

TEST_F(JitSse2NativeTest, RegisterAliasReadsOriginalSource) {
    const std::vector<u8> code{
        0x66, 0x0F, 0xD5, 0xC0}; // PMULLW XMM0, XMM0
    const CpuState interpreted = run_interpreter(code);
    const CpuState jitted = run_jit(code, false);
    expect_same_result(interpreted, jitted, code.size());
}

// Counting matching bytes is the one SSE2 sequence whose result feeds integer
// control flow rather than another vector lane, so a JIT/interpreter split here
// is invisible until a caller compares the vector count against a scalar count
// of the same bytes and disagrees with itself. Both lowerings compilers emit
// are pinned: fold-with-PSADBW and extract-mask-then-POPCNT.
class JitSse2ByteCountTest : public JitSse2NativeTest {
protected:
    static constexpr u8 kNeedle = '_';

    CpuState count_cpu() const {
        CpuState cpu;
        cpu.init_protected_mode();
        cpu.gpr.set32(EDX, kData);
        cpu.gpr.set32(ESP, 0x10000);
        cpu.flags.rflags = kInitialFlags;
        cpu.rip = kCode;
        std::memset(cpu.sse.xmm[0].u8_, 0, sizeof(XmmReg));
        std::memset(cpu.sse.xmm[2].u8_, 0, sizeof(XmmReg));
        std::memset(cpu.sse.xmm[3].u8_, 0, sizeof(XmmReg));
        for (int i = 0; i < 16; ++i) cpu.sse.xmm[1].u8_[i] = kNeedle;
        return cpu;
    }

    CpuState run_both_and_compare(const std::vector<u8>& code,
                                  const std::string& data) {
        EXPECT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
        EXPECT_TRUE(m_memory->write_bytes(
            kData, reinterpret_cast<const u8*>(data.data()), data.size()));

        CpuState interpreted = count_cpu();
        Interpreter interpreter(interpreted, *m_memory);
        EXPECT_EQ(interpreter.run(4), ExecResult::Continue);

        BasicBlock block = decode_block(code);
        CpuState jitted = count_cpu();
        JitCompiler jit(*m_memory);
        CompiledBlock* compiled = jit.compile(block);
        EXPECT_NE(compiled, nullptr);
        if (compiled) {
            EXPECT_EQ(jit.execute(*compiled, jitted), JitExecResult::Continue);
        }

        EXPECT_EQ(jitted.gpr.get32(0), interpreted.gpr.get32(0));
        EXPECT_EQ(jitted.sse.xmm[2].u64_[0], interpreted.sse.xmm[2].u64_[0]);
        EXPECT_EQ(jitted.sse.xmm[2].u64_[1], interpreted.sse.xmm[2].u64_[1]);
        return jitted;
    }

    static u64 scalar_count(const std::string& data) {
        return static_cast<u64>(std::count(
            data.begin(), data.end(), static_cast<char>(kNeedle)));
    }
};

TEST_F(JitSse2ByteCountTest, PsadbwFoldMatchesScalarCountInBothEngines) {
    const std::vector<u8> code{
        0xF3, 0x0F, 0x6F, 0x02,   // MOVDQU XMM0, [EDX]
        0x66, 0x0F, 0x74, 0xC1,   // PCMPEQB XMM0, XMM1
        0x66, 0x0F, 0xF8, 0xD0,   // PSUBB XMM2, XMM0
        0x66, 0x0F, 0xF6, 0xD3,   // PSADBW XMM2, XMM3
    };

    const std::vector<std::string> inputs = {
        std::string(16, 'a'),
        std::string(16, '_'),
        "a_b_c_d_e_f_g_h_",
        "spotify_track_id",
        std::string("\x80\xFF_\x7F\x00_zz\xC0_aa\xFF__z", 16),
    };

    for (const std::string& input : inputs) {
        SCOPED_TRACE(input);
        const CpuState result = run_both_and_compare(code, input);
        EXPECT_EQ(result.sse.xmm[2].u64_[0] + result.sse.xmm[2].u64_[1],
                  scalar_count(input));
    }
}

TEST_F(JitSse2ByteCountTest, MaskPopcountMatchesScalarCountInBothEngines) {
    const std::vector<u8> code{
        0xF3, 0x0F, 0x6F, 0x02,   // MOVDQU XMM0, [EDX]
        0x66, 0x0F, 0x74, 0xC1,   // PCMPEQB XMM0, XMM1
        0x66, 0x0F, 0xD7, 0xC0,   // PMOVMSKB EAX, XMM0
        0xF3, 0x0F, 0xB8, 0xC0,   // POPCNT EAX, EAX
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
        SCOPED_TRACE(input);
        const CpuState result = run_both_and_compare(code, input);
        EXPECT_EQ(result.gpr.get32(0), scalar_count(input));
    }
}

TEST_F(JitSse2NativeTest, MemoryFormRetainsCheckedHelperFallback) {
    const CpuState initial = initial_cpu();
    ASSERT_TRUE(m_memory->write_bytes(
        kData, initial.sse.xmm[1].u8_, sizeof(XmmReg)));
    const std::vector<u8> code{
        0x66, 0x0F, 0xDC, 0x02}; // PADDUSB XMM0, [EDX]

    const CpuState interpreted = run_interpreter(code);
    const CpuState jitted = run_jit(code, true);
    expect_same_result(interpreted, jitted, code.size());
}

TEST_F(JitSse2NativeTest, ShufpsMemMatchesInterpreter) {
    // 0F C6 02 1B — SHUFPS xmm0, [edx], 0x1B (dump 0x509753 / 0xc2b53f family)
    const std::vector<u8> code{0x0F, 0xC6, 0x02, 0x1B};
    const CpuState initial = initial_cpu();
    ASSERT_TRUE(m_memory->write_bytes(
        kData, initial.sse.xmm[1].u8_, sizeof(XmmReg)));
    const CpuState interpreted = run_interpreter(code);
    const CpuState jitted = run_jit(code, true);
    expect_same_result(interpreted, jitted, code.size());
}

TEST_F(JitSse2NativeTest, ShufpsDumpImmediatesMatchWithoutHelper) {
    // Sims load hot SHUFPS immediates: 0x00/0x08/0xAA/0xB1/0x1B.
    const std::array<u8, 5> imms{{0x00, 0x08, 0xAA, 0xB1, 0x1B}};
    for (u8 imm : imms) {
        SCOPED_TRACE(static_cast<int>(imm));
        const std::vector<u8> code{0x0F, 0xC6, 0xC1, imm};
        const CpuState interpreted = run_interpreter(code);
        const CpuState jitted = run_jit(code, false);
        expect_same_result(interpreted, jitted, code.size());
    }
}

TEST_F(JitSse2NativeTest, PslldXmmRegMatchesInterpreterWithoutHelper) {
    // 66 0F F2 C1 — PSLLD xmm0, xmm1. Count in low qword of src.
    const std::vector<u8> code{0x66, 0x0F, 0xF2, 0xC1};
    auto run_count = [&](u64 count) {
        EXPECT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
        CpuState interpreted = initial_cpu();
        interpreted.sse.xmm[1].u64_[0] = count;
        interpreted.sse.xmm[1].u64_[1] = 0xDEADBEEFCAFEBABEull;
        {
            Interpreter interpreter(interpreted, *m_memory);
            EXPECT_EQ(interpreter.run(1), ExecResult::Continue);
        }
        CpuState jitted = initial_cpu();
        jitted.sse.xmm[1].u64_[0] = count;
        jitted.sse.xmm[1].u64_[1] = 0xDEADBEEFCAFEBABEull;
        BasicBlock block = decode_block(code);
        ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
        JitCompiler jit(*m_memory);
        CompiledBlock* compiled = jit.compile(block);
        ASSERT_NE(compiled, nullptr);
        bool has_blr_x16 = false;
        const auto* code_bytes = static_cast<const u8*>(compiled->code);
        for (u32 offset = 0;
             offset + sizeof(u32) <= compiled->code_size;
             offset += sizeof(u32)) {
            u32 word = 0;
            std::memcpy(&word, code_bytes + offset, sizeof(word));
            has_blr_x16 |= (word == kBlrX16);
        }
        EXPECT_FALSE(has_blr_x16);
        EXPECT_EQ(jit.execute(*compiled, jitted), JitExecResult::Continue);
        expect_same_result(interpreted, jitted, code.size());
    };
    run_count(2);
    run_count(31);
    run_count(32);
}

TEST_F(JitSse2NativeTest, PsrldXmmRegMatchesInterpreterWithoutHelper) {
    const std::vector<u8> code{0x66, 0x0F, 0xD2, 0xC1}; // PSRLD xmm0, xmm1
    EXPECT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
    CpuState interpreted = initial_cpu();
    interpreted.sse.xmm[1].u64_[0] = 4;
    interpreted.sse.xmm[1].u64_[1] = 0;
    {
        Interpreter interpreter(interpreted, *m_memory);
        EXPECT_EQ(interpreter.run(1), ExecResult::Continue);
    }
    CpuState jitted = initial_cpu();
    jitted.sse.xmm[1].u64_[0] = 4;
    jitted.sse.xmm[1].u64_[1] = 0;
    BasicBlock block = decode_block(code);
    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    JitCompiler jit(*m_memory);
    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr);
    bool has_blr_x16 = false;
    const auto* code_bytes = static_cast<const u8*>(compiled->code);
    for (u32 offset = 0;
         offset + sizeof(u32) <= compiled->code_size;
         offset += sizeof(u32)) {
        u32 word = 0;
        std::memcpy(&word, code_bytes + offset, sizeof(word));
        has_blr_x16 |= (word == kBlrX16);
    }
    EXPECT_FALSE(has_blr_x16);
    EXPECT_EQ(jit.execute(*compiled, jitted), JitExecResult::Continue);
    expect_same_result(interpreted, jitted, code.size());
}

} // namespace
