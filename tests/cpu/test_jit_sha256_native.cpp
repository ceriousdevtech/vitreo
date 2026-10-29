// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Direct ARMv8 SHA-256 message-schedule JIT contract tests
//
// SHA256MSG1/SHA256MSG2 register forms are emitted as ARMv8 SHA256SU0/SHA256SU1
// instead of dispatching jit_sse_op. These tests pin the encodings, prove
// interpreter/JIT parity for both 32-bit and 64-bit blocks, prove the helper
// call is really gone, and prove the memory-source forms still route through
// the checked helper.

#include <gtest/gtest.h>

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

// NP 0F 38 CC /r — SHA256MSG1 xmm1, xmm2/m128
// NP 0F 38 CD /r — SHA256MSG2 xmm1, xmm2/m128
constexpr u8 kMsg1Opcode = 0xCC;
constexpr u8 kMsg2Opcode = 0xCD;

class JitSha256NativeTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        ASSERT_TRUE(m_memory->map_region(
            kCode, 0x10000, MemoryProtection::ReadWrite));
    }

    // Message-schedule words chosen to exercise carry propagation, the sign
    // bit, and the rotate/shift boundaries of sigma0 and sigma1.
    static void seed_vectors(CpuState& cpu) {
        const std::array<u32, 4> dst{{
            0x00000000u, 0x00000001u, 0x80000000u, 0xFFFFFFFFu}};
        const std::array<u32, 4> src{{
            0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au}};
        std::memcpy(cpu.sse.xmm[0].u32_, dst.data(), sizeof(dst));
        std::memcpy(cpu.sse.xmm[1].u32_, src.data(), sizeof(src));
    }

    CpuState initial_cpu_32() const {
        CpuState cpu;
        cpu.init_protected_mode();
        cpu.gpr.set32(EDX, kData);
        cpu.gpr.set32(ESP, 0x10000);
        cpu.flags.rflags = kInitialFlags;
        cpu.rip = kCode;
        seed_vectors(cpu);
        return cpu;
    }

    CpuState initial_cpu_64() const {
        CpuState cpu;
        cpu.init_long_mode();
        cpu.gpr.set64(EDX, kData);
        cpu.gpr.set64(ESP, 0x10000);
        cpu.flags.rflags = kInitialFlags;
        cpu.rip = kCode;
        seed_vectors(cpu);
        return cpu;
    }

    BasicBlock decode_block(const std::vector<u8>& code, bool bits64) {
        BasicBlock block;
        block.start_addr = kCode;
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

    CpuState run_interpreter(const std::vector<u8>& code, bool bits64) {
        EXPECT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
        CpuState cpu = bits64 ? initial_cpu_64() : initial_cpu_32();
        Interpreter interpreter(cpu, *m_memory);
        EXPECT_EQ(interpreter.run(1), ExecResult::Continue);
        return cpu;
    }

    CpuState run_jit(const std::vector<u8>& code, bool expect_sse_helper_call,
                     bool bits64) {
        EXPECT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
        BasicBlock block = decode_block(code, bits64);
        CpuState cpu = bits64 ? initial_cpu_64() : initial_cpu_32();

        // Delegation off: the block must be compiled natively, not handed
        // back to the interpreter, for the "no helper call" claim to mean
        // anything.
        ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
        JitCompiler jit(*m_memory);
        CompiledBlock* compiled = jit.compile(block);
        EXPECT_NE(compiled, nullptr);
        if (!compiled) {
            return cpu;
        }

        bool has_blr_x16 = false;
        const auto* code_bytes = static_cast<const u8*>(compiled->code);
        for (u32 offset = 0; offset + sizeof(u32) <= compiled->code_size;
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
        EXPECT_EQ(jitted.sse.xmm[0].u64_[0], interpreted.sse.xmm[0].u64_[0]);
        EXPECT_EQ(jitted.sse.xmm[0].u64_[1], interpreted.sse.xmm[0].u64_[1]);
        // The source register must be untouched by either engine.
        EXPECT_EQ(jitted.sse.xmm[1].u64_[0], interpreted.sse.xmm[1].u64_[0]);
        EXPECT_EQ(jitted.sse.xmm[1].u64_[1], interpreted.sse.xmm[1].u64_[1]);
        // SHA256MSG1/2 do not affect EFLAGS.
        EXPECT_EQ(jitted.flags.rflags, kInitialFlags);
        EXPECT_EQ(interpreted.flags.rflags, kInitialFlags);
        EXPECT_EQ(jitted.rip, kCode + instruction_length);
        EXPECT_EQ(interpreted.rip, kCode + instruction_length);
    }

    std::unique_ptr<GuestMemory> m_memory;
};

// ---- Independent bitwise references (not the interpreter's own helpers) ----

u32 ror32_ref(u32 x, int n) { return (x >> n) | (x << (32 - n)); }
u32 sigma0_ref(u32 x) {
    return ror32_ref(x, 7) ^ ror32_ref(x, 18) ^ (x >> 3);
}
u32 sigma1_ref(u32 x) {
    return ror32_ref(x, 17) ^ ror32_ref(x, 19) ^ (x >> 10);
}

TEST(Arm64Sha256EncodingTest, MatchesAssemblerEncodings) {
    // Verified against Apple Clang's assembler:
    //   sha256su0 v3.4s, v4.4s        -> 0x5E282883
    //   sha256su1 v3.4s, v4.4s, v5.4s -> 0x5E056083
    //   ins       v3.d[1], v4.d[1]    -> 0x6E184483
    EXPECT_EQ(arm64::sha256su0(3, 4), 0x5E282883u);
    EXPECT_EQ(arm64::sha256su1(3, 4, 5), 0x5E056083u);
    EXPECT_EQ(arm64::ins_v_d1_from_d1(3, 4), 0x6E184483u);
}

TEST_F(JitSha256NativeTest, Msg1MatchesBitwiseReferenceWithoutHelperCall) {
    const std::vector<u8> code{0x0F, 0x38, kMsg1Opcode, 0xC1}; // MSG1 x0, x1
    const CpuState interpreted = run_interpreter(code, false);
    const CpuState jitted = run_jit(code, false, false);
    expect_same_result(interpreted, jitted, code.size());

    const CpuState initial = initial_cpu_32();
    const u32* d = initial.sse.xmm[0].u32_;
    const u32* s = initial.sse.xmm[1].u32_;
    const std::array<u32, 4> expected{{
        d[0] + sigma0_ref(d[1]),
        d[1] + sigma0_ref(d[2]),
        d[2] + sigma0_ref(d[3]),
        d[3] + sigma0_ref(s[0]),
    }};
    for (int i = 0; i < 4; ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(jitted.sse.xmm[0].u32_[i], expected[static_cast<usize>(i)]);
    }
}

TEST_F(JitSha256NativeTest, Msg2MatchesBitwiseReferenceWithoutHelperCall) {
    const std::vector<u8> code{0x0F, 0x38, kMsg2Opcode, 0xC1}; // MSG2 x0, x1
    const CpuState interpreted = run_interpreter(code, false);
    const CpuState jitted = run_jit(code, false, false);
    expect_same_result(interpreted, jitted, code.size());

    const CpuState initial = initial_cpu_32();
    const u32* d = initial.sse.xmm[0].u32_;
    const u32* s = initial.sse.xmm[1].u32_;
    std::array<u32, 4> expected{{d[0], d[1], d[2], d[3]}};
    expected[0] += sigma1_ref(s[2]);
    expected[1] += sigma1_ref(s[3]);
    expected[2] += sigma1_ref(expected[0]);
    expected[3] += sigma1_ref(expected[1]);
    for (int i = 0; i < 4; ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(jitted.sse.xmm[0].u32_[i], expected[static_cast<usize>(i)]);
    }
}

TEST_F(JitSha256NativeTest, Msg1And2MatchInterpreterIn64BitBlocks) {
    for (u8 opcode : {kMsg1Opcode, kMsg2Opcode}) {
        SCOPED_TRACE(static_cast<int>(opcode));
        const std::vector<u8> code{0x0F, 0x38, opcode, 0xC1};
        const CpuState interpreted = run_interpreter(code, true);
        const CpuState jitted = run_jit(code, false, true);
        expect_same_result(interpreted, jitted, code.size());
    }
}

TEST_F(JitSha256NativeTest, HighRegisterOperandsMatchInterpreter) {
    // REX.R/REX.B select XMM8..XMM15, whose save-area offsets are farther
    // from the CpuState base than the low registers'.
    // 44 0F 38 CC C1 -> SHA256MSG1 XMM8, XMM9
    const std::vector<u8> code{0x45, 0x0F, 0x38, kMsg1Opcode, 0xC1};
    EXPECT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));

    CpuState interpreted = initial_cpu_64();
    interpreted.sse.xmm[8] = interpreted.sse.xmm[0];
    interpreted.sse.xmm[9] = interpreted.sse.xmm[1];
    {
        Interpreter interpreter(interpreted, *m_memory);
        EXPECT_EQ(interpreter.run(1), ExecResult::Continue);
    }

    BasicBlock block = decode_block(code, true);
    CpuState jitted = initial_cpu_64();
    jitted.sse.xmm[8] = jitted.sse.xmm[0];
    jitted.sse.xmm[9] = jitted.sse.xmm[1];
    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    JitCompiler jit(*m_memory);
    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr);
    EXPECT_EQ(jit.execute(*compiled, jitted), JitExecResult::Continue);

    EXPECT_EQ(jitted.sse.xmm[8].u64_[0], interpreted.sse.xmm[8].u64_[0]);
    EXPECT_EQ(jitted.sse.xmm[8].u64_[1], interpreted.sse.xmm[8].u64_[1]);
    EXPECT_EQ(jitted.sse.xmm[9].u64_[0], interpreted.sse.xmm[9].u64_[0]);
    EXPECT_EQ(jitted.sse.xmm[9].u64_[1], interpreted.sse.xmm[9].u64_[1]);
}

TEST_F(JitSha256NativeTest, RegisterAliasReadsOriginalSource) {
    for (u8 opcode : {kMsg1Opcode, kMsg2Opcode}) {
        SCOPED_TRACE(static_cast<int>(opcode));
        const std::vector<u8> code{0x0F, 0x38, opcode, 0xC0}; // dst == src
        const CpuState interpreted = run_interpreter(code, false);
        const CpuState jitted = run_jit(code, false, false);
        expect_same_result(interpreted, jitted, code.size());
    }
}

TEST_F(JitSha256NativeTest, MemoryFormRetainsCheckedHelperFallback) {
    const CpuState initial = initial_cpu_32();
    ASSERT_TRUE(m_memory->write_bytes(
        kData, initial.sse.xmm[1].u8_, sizeof(XmmReg)));

    for (u8 opcode : {kMsg1Opcode, kMsg2Opcode}) {
        SCOPED_TRACE(static_cast<int>(opcode));
        const std::vector<u8> code{0x0F, 0x38, opcode, 0x02}; // src = [EDX]
        const CpuState interpreted = run_interpreter(code, false);
        const CpuState jitted = run_jit(code, true, false);
        expect_same_result(interpreted, jitted, code.size());
    }
}

TEST_F(JitSha256NativeTest, Rnds2StaysOnHelper) {
    // ARM's SHA256H performs four rounds where x86's SHA256RNDS2 performs
    // two, so this form has no direct equivalent and must keep its helper.
    const std::vector<u8> code{0x0F, 0x38, 0xCB, 0xC1};
    const CpuState interpreted = run_interpreter(code, false);
    const CpuState jitted = run_jit(code, true, false);
    expect_same_result(interpreted, jitted, code.size());
}

} // namespace
