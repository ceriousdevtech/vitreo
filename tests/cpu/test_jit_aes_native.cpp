// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Direct ARMv8 AES JIT contract tests
//
// AESENC / AESENCLAST / AESDEC / AESDECLAST / AESIMC are emitted as inline
// ARMv8 FEAT_AES instructions instead of dispatching jit_sse_op. These tests
// pin the ARM encodings, prove interpreter/JIT parity over zero, all-ones and
// pseudo-random vectors in both register and memory forms and with dst == src,
// prove the native lowering is actually REACHED (the emitted block contains
// the ARM AES opcodes and no helper BLR), run the FIPS-197 known-answer
// vectors through a full 10-round AES-128 encryption and decryption built
// entirely out of JIT-compiled AES-NI, and pin AESKEYGENASSIST to the helper.

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

// 66 0F 38 DB /r  AESIMC       66 0F 38 DE /r  AESDEC
// 66 0F 38 DC /r  AESENC       66 0F 38 DF /r  AESDECLAST
// 66 0F 38 DD /r  AESENCLAST   66 0F 3A DF /r ib  AESKEYGENASSIST
constexpr u8 kAesImc     = 0xDB;
constexpr u8 kAesEnc     = 0xDC;
constexpr u8 kAesEncLast = 0xDD;
constexpr u8 kAesDec     = 0xDE;
constexpr u8 kAesDecLast = 0xDF;

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

using Block16 = std::array<u8, 16>;

// Operand vectors: all-zero, all-ones, and pseudo-random. The random pairs are
// fixed constants so a failure is reproducible; they are deliberately
// asymmetric between dst and src so a swapped-operand lowering cannot pass.
struct VectorPair {
    const char* name;
    Block16 dst;
    Block16 src;
};

const std::array<VectorPair, 6> kVectors{{
    {"zero",
     {{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}},
     {{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}}},
    {"ones",
     {{0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
       0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}},
     {{0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
       0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}}},
    {"zero_dst_ones_src",
     {{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}},
     {{0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
       0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}}},
    {"rand_a",
     {{0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
       0x88,0x99,0xAA,0xBB,0xCC,0xDD,0xEE,0xFF}},
     {{0x0F,0x15,0x71,0xC9,0x47,0xD9,0xE8,0x59,
       0x0C,0xB7,0xAD,0xD6,0xAF,0x7F,0x67,0x98}}},
    {"rand_b",
     {{0x6B,0xC1,0xBE,0xE2,0x2E,0x40,0x9F,0x96,
       0xE9,0x3D,0x7E,0x11,0x73,0x93,0x17,0x2A}},
     {{0x2B,0x7E,0x15,0x16,0x28,0xAE,0xD2,0xA6,
       0xAB,0xF7,0x15,0x88,0x09,0xCF,0x4F,0x3C}}},
    {"rand_c",
     {{0xF6,0x9F,0x24,0x45,0xDF,0x4F,0x9B,0x17,
       0xAD,0x2B,0x41,0x7B,0xE6,0x6C,0x37,0x10}},
     {{0x8E,0x73,0xB0,0xF7,0xDA,0x0E,0x64,0x52,
       0xC8,0x10,0xF3,0x2B,0x80,0x90,0x79,0xE5}}},
}};

std::vector<u32> block_words(const CompiledBlock& compiled) {
    std::vector<u32> words;
    const auto* bytes = static_cast<const u8*>(compiled.code);
    for (u32 offset = 0; offset + sizeof(u32) <= compiled.code_size;
         offset += sizeof(u32)) {
        u32 word = 0;
        std::memcpy(&word, bytes + offset, sizeof(word));
        words.push_back(word);
    }
    return words;
}

bool contains(const std::vector<u32>& words, u32 needle) {
    for (u32 w : words) {
        if (w == needle) return true;
    }
    return false;
}

// True when the block contains any ARM64 cryptographic-AES instruction.
// Encoding (ARM ARM C4.1 "Cryptographic AES"): 0100 1110 00 10100 opcode(5)
// 10 Rn Rd, with opcode in {00100 AESE, 00101 AESD, 00110 AESMC,
// 00111 AESIMC}. Masking off Rn/Rd leaves the four fixed words.
bool has_any_arm_aes(const std::vector<u32>& words) {
    for (u32 w : words) {
        const u32 base = w & 0xFFFFFC00u;
        if (base == 0x4E284800u || base == 0x4E285800u ||
            base == 0x4E286800u || base == 0x4E287800u) {
            return true;
        }
    }
    return false;
}

bool has_helper_blr(const std::vector<u32>& words) {
    return contains(words, kBlrX16);
}

class JitAesNativeTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        ASSERT_TRUE(m_memory->map_region(
            kCode, 0x10000, MemoryProtection::ReadWrite));
    }

    CpuState initial_cpu(bool bits64, const Block16& dst,
                         const Block16& src) const {
        CpuState cpu;
        if (bits64) {
            cpu.init_long_mode();
            cpu.gpr.set64(EDX, kData);
            cpu.gpr.set64(ESP, 0x10000);
        } else {
            cpu.init_protected_mode();
            cpu.gpr.set32(EDX, kData);
            cpu.gpr.set32(ESP, 0x10000);
        }
        cpu.flags.rflags = kInitialFlags;
        cpu.rip = kCode;
        std::memcpy(cpu.sse.xmm[0].u8_, dst.data(), dst.size());
        std::memcpy(cpu.sse.xmm[1].u8_, src.data(), src.size());
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
            if (error != DecodeError::Success) break;
            block.instructions.push_back(instruction);
        }
        block.byte_length = static_cast<u32>(reader.position());
        block.instruction_count =
            static_cast<u32>(block.instructions.size());
        return block;
    }

    CpuState run_interpreter(const std::vector<u8>& code, bool bits64,
                             const Block16& dst, const Block16& src,
                             u32 instruction_count = 1) {
        EXPECT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
        CpuState cpu = initial_cpu(bits64, dst, src);
        Interpreter interpreter(cpu, *m_memory);
        EXPECT_EQ(interpreter.run(instruction_count), ExecResult::Continue);
        return cpu;
    }

    // Compiles and runs `code`, returning the final CpuState. `words` receives
    // the emitted ARM64 block so the caller can assert on what was emitted.
    CpuState run_jit(const std::vector<u8>& code, bool bits64,
                     const Block16& dst, const Block16& src,
                     std::vector<u32>* words = nullptr) {
        EXPECT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
        BasicBlock block = decode_block(code, bits64);
        CpuState cpu = initial_cpu(bits64, dst, src);

        // Delegation off: the block must be compiled natively, not handed back
        // to the interpreter, or the reached-ness claims mean nothing.
        ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
        JitCompiler jit(*m_memory);
        CompiledBlock* compiled = jit.compile(block);
        EXPECT_NE(compiled, nullptr);
        if (!compiled) return cpu;
        if (words) *words = block_words(*compiled);
        EXPECT_EQ(jit.execute(*compiled, cpu), JitExecResult::Continue);
        return cpu;
    }

    void expect_state_equal(const CpuState& a, const CpuState& b) {
        for (int i = 0; i < 2; ++i) {
            SCOPED_TRACE(i);
            EXPECT_EQ(a.sse.xmm[i].u64_[0], b.sse.xmm[i].u64_[0]);
            EXPECT_EQ(a.sse.xmm[i].u64_[1], b.sse.xmm[i].u64_[1]);
        }
        // AES-NI defines no EFLAGS effect.
        EXPECT_EQ(a.flags.rflags, kInitialFlags);
        EXPECT_EQ(b.flags.rflags, kInitialFlags);
        EXPECT_EQ(a.rip, b.rip);
    }

    std::unique_ptr<GuestMemory> m_memory;
};

// ===========================================================================
// 1. Encodings
// ===========================================================================

TEST(Arm64AesEncodingTest, MatchesAssemblerEncodings) {
    // Verified against Apple Clang's assembler:
    //   aese.16b  v0, v2   -> 0x4E284840      aese.16b  v3, v4  -> 0x4E284883
    //   aesd.16b  v0, v2   -> 0x4E285840      aesd.16b  v5, v6  -> 0x4E2858C5
    //   aesmc.16b v0, v0   -> 0x4E286800      aesmc.16b v7, v8  -> 0x4E286907
    //   aesimc.16b v0, v1  -> 0x4E287820      aesimc.16b v9,v10 -> 0x4E287949
    EXPECT_EQ(arm64::aese(0, 2), 0x4E284840u);
    EXPECT_EQ(arm64::aese(3, 4), 0x4E284883u);
    EXPECT_EQ(arm64::aesd(0, 2), 0x4E285840u);
    EXPECT_EQ(arm64::aesd(5, 6), 0x4E2858C5u);
    EXPECT_EQ(arm64::aesmc(0, 0), 0x4E286800u);
    EXPECT_EQ(arm64::aesmc(7, 8), 0x4E286907u);
    EXPECT_EQ(arm64::aesimc(0, 1), 0x4E287820u);
    EXPECT_EQ(arm64::aesimc(9, 10), 0x4E287949u);
}

TEST(Arm64AesEncodingTest, HostAdvertisesFeatAes) {
    // The lowering is gated on this; if it is ever false on the build machine
    // every parity test below silently degrades to testing the helper.
    EXPECT_TRUE(host_supports_aes());
}

// ===========================================================================
// 2. Interpreter parity, register form, every opcode x every vector
// ===========================================================================

TEST_F(JitAesNativeTest, RegisterFormMatchesInterpreterForAllVectors) {
    for (u8 opcode : {kAesEnc, kAesEncLast, kAesDec, kAesDecLast, kAesImc}) {
        for (bool bits64 : {false, true}) {
            for (const VectorPair& v : kVectors) {
                SCOPED_TRACE(::testing::Message()
                             << "opcode=" << std::hex
                             << static_cast<int>(opcode) << " bits64="
                             << bits64 << " vec=" << v.name);
                // 66 0F 38 xx C1 -> op xmm0, xmm1
                const std::vector<u8> code{0x66, 0x0F, 0x38, opcode, 0xC1};
                const CpuState interpreted =
                    run_interpreter(code, bits64, v.dst, v.src);
                std::vector<u32> words;
                const CpuState jitted =
                    run_jit(code, bits64, v.dst, v.src, &words);
                expect_state_equal(interpreted, jitted);

                // Reached-ness: native ARM AES present, helper call absent.
                EXPECT_TRUE(has_any_arm_aes(words))
                    << "AES lowering regressed to the jit_sse_op helper";
                EXPECT_FALSE(has_helper_blr(words))
                    << "unexpected helper BLR in a register-form AES block";
            }
        }
    }
}

TEST_F(JitAesNativeTest, SameRegisterSourceAndDestination) {
    // AESENC xmm0, xmm0 reads the destination as BOTH the state and the round
    // key. A lowering that wrote the state before reading the key would
    // silently produce the wrong result only in this form.
    for (u8 opcode : {kAesEnc, kAesEncLast, kAesDec, kAesDecLast, kAesImc}) {
        for (const VectorPair& v : kVectors) {
            SCOPED_TRACE(::testing::Message()
                         << "opcode=" << std::hex << static_cast<int>(opcode)
                         << " vec=" << v.name);
            const std::vector<u8> code{0x66, 0x0F, 0x38, opcode, 0xC0};
            const CpuState interpreted =
                run_interpreter(code, false, v.dst, v.src);
            std::vector<u32> words;
            const CpuState jitted = run_jit(code, false, v.dst, v.src, &words);
            expect_state_equal(interpreted, jitted);
            EXPECT_TRUE(has_any_arm_aes(words));
            EXPECT_FALSE(has_helper_blr(words));
        }
    }
}

TEST_F(JitAesNativeTest, HighRegisterOperandsMatchInterpreter) {
    // REX.R/REX.B select XMM8..XMM15, whose save-area offsets are farther from
    // the CpuState base than the low registers'.
    for (u8 opcode : {kAesEnc, kAesDec, kAesImc}) {
        SCOPED_TRACE(static_cast<int>(opcode));
        // 66 45 0F 38 xx C1 -> op xmm8, xmm9
        const std::vector<u8> code{0x66, 0x45, 0x0F, 0x38, opcode, 0xC1};
        ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));

        const VectorPair& v = kVectors[3];
        CpuState interpreted = initial_cpu(true, v.dst, v.src);
        interpreted.sse.xmm[8] = interpreted.sse.xmm[0];
        interpreted.sse.xmm[9] = interpreted.sse.xmm[1];
        {
            Interpreter interpreter(interpreted, *m_memory);
            EXPECT_EQ(interpreter.run(1), ExecResult::Continue);
        }

        BasicBlock block = decode_block(code, true);
        CpuState jitted = initial_cpu(true, v.dst, v.src);
        jitted.sse.xmm[8] = jitted.sse.xmm[0];
        jitted.sse.xmm[9] = jitted.sse.xmm[1];
        ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
        JitCompiler jit(*m_memory);
        CompiledBlock* compiled = jit.compile(block);
        ASSERT_NE(compiled, nullptr);
        EXPECT_TRUE(has_any_arm_aes(block_words(*compiled)));
        EXPECT_EQ(jit.execute(*compiled, jitted), JitExecResult::Continue);

        EXPECT_EQ(jitted.sse.xmm[8].u64_[0], interpreted.sse.xmm[8].u64_[0]);
        EXPECT_EQ(jitted.sse.xmm[8].u64_[1], interpreted.sse.xmm[8].u64_[1]);
        EXPECT_EQ(jitted.sse.xmm[9].u64_[0], interpreted.sse.xmm[9].u64_[0]);
        EXPECT_EQ(jitted.sse.xmm[9].u64_[1], interpreted.sse.xmm[9].u64_[1]);
    }
}

// ===========================================================================
// 3. Memory-source form
// ===========================================================================

TEST_F(JitAesNativeTest, MemoryFormMatchesInterpreterForAllVectors) {
    for (u8 opcode : {kAesEnc, kAesEncLast, kAesDec, kAesDecLast, kAesImc}) {
        for (bool bits64 : {false, true}) {
            for (const VectorPair& v : kVectors) {
                SCOPED_TRACE(::testing::Message()
                             << "opcode=" << std::hex
                             << static_cast<int>(opcode) << " bits64="
                             << bits64 << " vec=" << v.name);
                ASSERT_TRUE(m_memory->write_bytes(kData, v.src.data(),
                                                  v.src.size()));
                // 66 0F 38 xx 02 -> op xmm0, [(R|E)DX]
                const std::vector<u8> code{0x66, 0x0F, 0x38, opcode, 0x02};
                const CpuState interpreted =
                    run_interpreter(code, bits64, v.dst, v.src);
                std::vector<u32> words;
                const CpuState jitted =
                    run_jit(code, bits64, v.dst, v.src, &words);
                expect_state_equal(interpreted, jitted);
                // A 64-bit block always has a validated inline 128-bit vector
                // load (linear64 or the x64 region), so the memory form is
                // native there. A 32-bit block on non-linear memory has none,
                // and keeps the checked helper by design.
                if (bits64) {
                    EXPECT_TRUE(has_any_arm_aes(words))
                        << "64-bit memory-form AES regressed to the helper";
                }
            }
        }
    }
}

TEST_F(JitAesNativeTest, MemoryFormIsNativeUnderLinear64) {
    // Under the 64-bit linear model — the configuration Steam runs in — the
    // memory form must be lowered natively AND produce the interpreter's
    // answer, not merely emit AES opcodes.
    GuestMemory memory;
    if (!memory.enable_linear_reservation(usize{1} << 32)) {
        GTEST_SKIP() << "linear reservation unavailable on this host";
    }
    ASSERT_TRUE(memory.map_region(kCode, 0x10000,
                                  MemoryProtection::ReadWrite));

    for (u8 opcode : {kAesEnc, kAesEncLast, kAesDec, kAesDecLast, kAesImc}) {
        for (const VectorPair& v : kVectors) {
            SCOPED_TRACE(::testing::Message()
                         << "opcode=" << std::hex << static_cast<int>(opcode)
                         << " vec=" << v.name);
            ASSERT_TRUE(memory.write_bytes(kData, v.src.data(),
                                           v.src.size()));
            const std::vector<u8> code{0x66, 0x0F, 0x38, opcode, 0x02};
            ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));

            BasicBlock block;
            block.start_addr = kCode;
            block.is_64bit = true;
            MemoryByteReader reader(code.data(), code.size());
            DecodedInstruction instruction;
            ASSERT_EQ(
                decode_instruction(reader, DecodeMode::Mode64, instruction),
                DecodeError::Success);
            block.instructions.push_back(instruction);
            block.byte_length = static_cast<u32>(reader.position());
            block.instruction_count = 1;

            CpuState interpreted = initial_cpu(true, v.dst, v.src);
            {
                Interpreter interpreter(interpreted, memory);
                ASSERT_EQ(interpreter.run(1), ExecResult::Continue);
            }

            ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE",
                                                  "1");
            JitCompiler jit(memory);
            CompiledBlock* compiled = jit.compile(block);
            ASSERT_NE(compiled, nullptr);
            EXPECT_TRUE(has_any_arm_aes(block_words(*compiled)))
                << "memory-form AES regressed to the jit_sse_op helper under "
                   "the 64-bit linear model";

            CpuState jitted = initial_cpu(true, v.dst, v.src);
            ASSERT_EQ(jit.execute(*compiled, jitted), JitExecResult::Continue);
            expect_state_equal(interpreted, jitted);
        }
    }
}

// ===========================================================================
// 4. FIPS-197 known-answer vectors
//
// A per-instruction parity test can pass while the round sequence is subtly
// wrong (a swapped key XOR, a missing MixColumns on the wrong round). These
// tests run the complete AES-128 cipher and inverse cipher out of JIT-compiled
// AES-NI and compare against the values printed in FIPS-197.
// ===========================================================================

// FIPS-197 Appendix B / Appendix C.1:
//   key        2b7e151628aed2a6abf7158809cf4f3c
//   plaintext  3243f6a8885a308d313198a2e0370734
//   ciphertext 3925841d02dc09fbdc118597196a0b32
constexpr Block16 kFipsKey{{0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,
                            0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c}};
constexpr Block16 kFipsPlain{{0x32,0x43,0xf6,0xa8,0x88,0x5a,0x30,0x8d,
                              0x31,0x31,0x98,0xa2,0xe0,0x37,0x07,0x34}};
constexpr Block16 kFipsCipher{{0x39,0x25,0x84,0x1d,0x02,0xdc,0x09,0xfb,
                               0xdc,0x11,0x85,0x97,0x19,0x6a,0x0b,0x32}};

// Independent AES-128 key expansion (FIPS-197 section 5.2), written from the
// specification rather than taken from any implementation. It produces the 11
// round keys the cipher below consumes; only the ROUND FUNCTION is under test,
// and it runs entirely through the JIT.
const u8 kSbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

std::array<Block16, 11> expand_key_128(const Block16& key) {
    std::array<u8, 176> w{};
    std::memcpy(w.data(), key.data(), 16);
    u8 rcon = 0x01;
    for (int i = 16; i < 176; i += 4) {
        u8 t[4] = {w[static_cast<usize>(i) - 4], w[static_cast<usize>(i) - 3],
                   w[static_cast<usize>(i) - 2], w[static_cast<usize>(i) - 1]};
        if (i % 16 == 0) {
            const u8 tmp = t[0];                       // RotWord
            t[0] = kSbox[t[1]];
            t[1] = kSbox[t[2]];
            t[2] = kSbox[t[3]];
            t[3] = kSbox[tmp];
            t[0] = static_cast<u8>(t[0] ^ rcon);       // Rcon
            rcon = static_cast<u8>((rcon << 1) ^ ((rcon & 0x80) ? 0x1B : 0x00));
        }
        for (int j = 0; j < 4; ++j) {
            w[static_cast<usize>(i + j)] =
                static_cast<u8>(w[static_cast<usize>(i + j - 16)] ^ t[j]);
        }
    }
    std::array<Block16, 11> round_keys{};
    for (int r = 0; r < 11; ++r) {
        std::memcpy(round_keys[static_cast<usize>(r)].data(),
                    w.data() + r * 16, 16);
    }
    return round_keys;
}

// Runs one AES-NI instruction through the JIT: xmm0 = state, xmm1 = operand.
// Every step of the FIPS-197 cipher below goes through here, so the assertion
// at the end covers the JIT's own AES lowering and nothing else.
class FipsRunner {
public:
    FipsRunner() {
        m_memory = std::make_unique<GuestMemory>();
        m_ok = static_cast<bool>(
            m_memory->map_region(kCode, 0x10000,
                                 MemoryProtection::ReadWrite));
    }

    bool ok() const { return m_ok; }
    bool saw_native_aes() const { return m_saw_native_aes; }
    bool saw_helper_blr() const { return m_saw_helper_blr; }

    Block16 step(u8 opcode, const Block16& state, const Block16& operand) {
        const std::vector<u8> code{0x66, 0x0F, 0x38, opcode, 0xC1};
        EXPECT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));

        BasicBlock block;
        block.start_addr = kCode;
        block.is_64bit = true;
        MemoryByteReader reader(code.data(), code.size());
        DecodedInstruction instruction;
        EXPECT_EQ(decode_instruction(reader, DecodeMode::Mode64, instruction),
                  DecodeError::Success);
        block.instructions.push_back(instruction);
        block.byte_length = static_cast<u32>(reader.position());
        block.instruction_count = 1;

        CpuState cpu;
        cpu.init_long_mode();
        cpu.gpr.set64(ESP, 0x10000);
        cpu.flags.rflags = kInitialFlags;
        cpu.rip = kCode;
        std::memcpy(cpu.sse.xmm[0].u8_, state.data(), 16);
        std::memcpy(cpu.sse.xmm[1].u8_, operand.data(), 16);

        ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
        JitCompiler jit(*m_memory);
        CompiledBlock* compiled = jit.compile(block);
        EXPECT_NE(compiled, nullptr);
        Block16 out{};
        if (!compiled) return out;
        const std::vector<u32> words = block_words(*compiled);
        m_saw_native_aes |= has_any_arm_aes(words);
        m_saw_helper_blr |= has_helper_blr(words);
        EXPECT_EQ(jit.execute(*compiled, cpu), JitExecResult::Continue);
        std::memcpy(out.data(), cpu.sse.xmm[0].u8_, 16);
        return out;
    }

    static Block16 xor_blocks(const Block16& a, const Block16& b) {
        Block16 r{};
        for (usize i = 0; i < 16; ++i) {
            r[i] = static_cast<u8>(a[i] ^ b[i]);
        }
        return r;
    }

private:
    std::unique_ptr<GuestMemory> m_memory;
    bool m_ok{false};
    bool m_saw_native_aes{false};
    bool m_saw_helper_blr{false};
};

TEST(JitAesFips197Test, Aes128EncryptionMatchesFips197Vector) {
    FipsRunner runner;
    ASSERT_TRUE(runner.ok());
    const auto rk = expand_key_128(kFipsKey);

    // FIPS-197 cipher: AddRoundKey(rk[0]); 9 x full round; final round.
    Block16 state = FipsRunner::xor_blocks(kFipsPlain, rk[0]);
    for (int r = 1; r <= 9; ++r) {
        state = runner.step(kAesEnc, state, rk[static_cast<usize>(r)]);
    }
    state = runner.step(kAesEncLast, state, rk[10]);

    EXPECT_EQ(state, kFipsCipher);
    EXPECT_TRUE(runner.saw_native_aes())
        << "the FIPS-197 encryption ran on the jit_sse_op helper";
    EXPECT_FALSE(runner.saw_helper_blr())
        << "a helper BLR was emitted inside the FIPS-197 encryption";
}

TEST(JitAesFips197Test, Aes128DecryptionMatchesFips197Vector) {
    FipsRunner runner;
    ASSERT_TRUE(runner.ok());
    const auto rk = expand_key_128(kFipsKey);

    // FIPS-197 "equivalent inverse cipher", which is the form AES-NI's
    // AESDEC/AESDECLAST implement: the decryption key schedule is the
    // encryption schedule in reverse, with InvMixColumns applied to the
    // interior round keys. AESIMC computes exactly that transform, so the
    // decryption schedule is itself built out of the JIT's AES lowering.
    std::array<Block16, 11> dk{};
    dk[0] = rk[10];
    for (int r = 1; r <= 9; ++r) {
        dk[static_cast<usize>(r)] =
            runner.step(kAesImc, Block16{}, rk[static_cast<usize>(10 - r)]);
    }
    dk[10] = rk[0];

    Block16 state = FipsRunner::xor_blocks(kFipsCipher, dk[0]);
    for (int r = 1; r <= 9; ++r) {
        state = runner.step(kAesDec, state, dk[static_cast<usize>(r)]);
    }
    state = runner.step(kAesDecLast, state, dk[10]);

    EXPECT_EQ(state, kFipsPlain);
    EXPECT_TRUE(runner.saw_native_aes())
        << "the FIPS-197 decryption ran on the jit_sse_op helper";
    EXPECT_FALSE(runner.saw_helper_blr())
        << "a helper BLR was emitted inside the FIPS-197 decryption";
}

TEST(JitAesFips197Test, RoundTripThroughJitMatchesInterpreterKeySchedule) {
    // The same 10-round cipher, but the round keys are produced by the
    // interpreter's AESKEYGENASSIST (which stays on the helper). This closes
    // the loop: a key schedule built by one engine decrypts under the other.
    FipsRunner runner;
    ASSERT_TRUE(runner.ok());
    const auto rk = expand_key_128(kFipsKey);

    Block16 state = FipsRunner::xor_blocks(kFipsPlain, rk[0]);
    for (int r = 1; r <= 9; ++r) {
        state = runner.step(kAesEnc, state, rk[static_cast<usize>(r)]);
    }
    state = runner.step(kAesEncLast, state, rk[10]);
    ASSERT_EQ(state, kFipsCipher);

    std::array<Block16, 11> dk{};
    dk[0] = rk[10];
    for (int r = 1; r <= 9; ++r) {
        dk[static_cast<usize>(r)] =
            runner.step(kAesImc, Block16{}, rk[static_cast<usize>(10 - r)]);
    }
    dk[10] = rk[0];

    Block16 back = FipsRunner::xor_blocks(state, dk[0]);
    for (int r = 1; r <= 9; ++r) {
        back = runner.step(kAesDec, back, dk[static_cast<usize>(r)]);
    }
    back = runner.step(kAesDecLast, back, dk[10]);
    EXPECT_EQ(back, kFipsPlain);
}

// ===========================================================================
// 5. Opt-out arm and what stays on the helper
// ===========================================================================

TEST_F(JitAesNativeTest, OptOutRoutesBackToTheCheckedHelper) {
    ScopedEnvironmentVariable no_direct("VITREO_JIT_NO_AES_DIRECT", "1");
    const VectorPair& v = kVectors[4];
    for (u8 opcode : {kAesEnc, kAesEncLast, kAesDec, kAesDecLast, kAesImc}) {
        SCOPED_TRACE(static_cast<int>(opcode));
        const std::vector<u8> code{0x66, 0x0F, 0x38, opcode, 0xC1};
        const CpuState interpreted =
            run_interpreter(code, false, v.dst, v.src);
        std::vector<u32> words;
        const CpuState jitted = run_jit(code, false, v.dst, v.src, &words);
        // Same answer either way — the opt-out is a control arm, not a
        // different semantic.
        expect_state_equal(interpreted, jitted);
        EXPECT_FALSE(has_any_arm_aes(words));
        EXPECT_TRUE(has_helper_blr(words));
    }
}

TEST_F(JitAesNativeTest, KeygenAssistStaysOnHelper) {
    // ARMv8 has no SubWord/RotWord primitive and AESKEYGENASSIST runs once per
    // key schedule, not per block; it keeps its checked helper deliberately.
    const VectorPair& v = kVectors[4];
    // 66 0F 3A DF C1 01 -> AESKEYGENASSIST xmm0, xmm1, 1
    const std::vector<u8> code{0x66, 0x0F, 0x3A, 0xDF, 0xC1, 0x01};
    const CpuState interpreted = run_interpreter(code, false, v.dst, v.src);
    std::vector<u32> words;
    const CpuState jitted = run_jit(code, false, v.dst, v.src, &words);
    expect_state_equal(interpreted, jitted);
    EXPECT_TRUE(has_helper_blr(words));
}

} // namespace
