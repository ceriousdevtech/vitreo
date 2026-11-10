// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Direct ARMv8 SHA-256 message-schedule JIT contract tests
//
// SHA256MSG1/SHA256MSG2 register forms are emitted as ARMv8 SHA256SU0/SHA256SU1
// instead of dispatching jit_sse_op. These tests pin the encodings, prove
// interpreter/JIT parity for both 32-bit and 64-bit blocks, and prove the
// helper call is really gone. WS3 adds SHA256RNDS2 (register and memory, on
// SHA256H/SHA256H2) and the MSG1/MSG2 memory forms, checked against a
// two-round reference written from the SDM and the FIPS 180-4 "abc" digest.

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
#include "jit_crypto_test_harness.hpp"

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

// ===========================================================================
// WS3: SHA256RNDS2 and the MSG1/MSG2 memory forms, direct on FEAT_SHA256
// ===========================================================================

namespace w = ws3test;

constexpr u32 kSha256K[64] = {  // FIPS 180-4 §4.2.2
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};

// Intel SDM Vol. 2 SHA256RNDS2 Operation, two rounds.
XmmReg ref_rnds2(const XmmReg& src1, const XmmReg& src2, const XmmReg& wk) {
    u32 a = src2.u32_[3], b = src2.u32_[2], e = src2.u32_[1], f = src2.u32_[0];
    u32 c = src1.u32_[3], d = src1.u32_[2], g = src1.u32_[1], h = src1.u32_[0];
    for (int i = 0; i < 2; ++i) {
        const u32 S1 = ror32_ref(e, 6) ^ ror32_ref(e, 11) ^ ror32_ref(e, 25);
        const u32 ch = (e & f) ^ (~e & g);
        const u32 S0 = ror32_ref(a, 2) ^ ror32_ref(a, 13) ^ ror32_ref(a, 22);
        const u32 maj = (a & b) ^ (a & c) ^ (b & c);
        const u32 t1 = h + S1 + ch + wk.u32_[i];
        const u32 na = t1 + S0 + maj;
        const u32 ne = d + t1;
        h = g; g = f; f = e; e = ne; d = c; c = b; b = a; a = na;
    }
    XmmReg out{};
    out.u32_[3] = a; out.u32_[2] = b; out.u32_[1] = e; out.u32_[0] = f;
    return out;
}

TEST(Arm64Sha256RoundEncodingTest, MatchesAssemblerEncodings) {
    // llvm-mc -triple=aarch64 -mattr=+sha2 -show-encoding:
    //   sha256h  q0, q1, v2.4s -> [0x20,0x40,0x02,0x5e]
    //   sha256h2 q3, q4, v2.4s -> [0x83,0x50,0x02,0x5e]
    EXPECT_EQ(arm64::sha256h(0, 1, 2), 0x5E024020u);
    EXPECT_EQ(arm64::sha256h2(3, 4, 2), 0x5E025083u);
    EXPECT_TRUE(host_supports_sha256());
}

TEST(JitSha256Ws3Test, Rnds2MatchesBitwiseReferenceWithoutHelperCall) {
    for (w::MemCfg cfg : {w::MemCfg::NonLinear32, w::MemCfg::Region64}) {
        w::CryptoRig rig;
        ASSERT_TRUE(rig.init(cfg));
        u64 seed = 0x5EED5A256ULL;
        for (int v = 0; v < 64; ++v) {
            SCOPED_TRACE(::testing::Message() << w::cfg_name(cfg) << " v=" << v);
            CpuState init = rig.base();
            for (int r = 0; r < 3; ++r) {
                w::set_xmm(init, r, w::next_rand(seed), w::next_rand(seed));
            }
            // SHA256RNDS2 xmm1, xmm2 (0F 38 CB CA), WK = xmm0.
            const XmmReg want12 =
                ref_rnds2(init.sse.xmm[1], init.sse.xmm[2], init.sse.xmm[0]);
            const w::JitRun r1 =
                w::expect_parity(rig, {0x0F, 0x38, 0xCB, 0xCA}, init);
            EXPECT_EQ(r1.cpu.sse.xmm[1].u64_[0], want12.u64_[0]);
            EXPECT_EQ(r1.cpu.sse.xmm[1].u64_[1], want12.u64_[1]);
            EXPECT_FALSE(w::has_sse_op_call(r1.words));
        }
    }
}

TEST(JitSha256Ws3Test, Rnds2AliasForms) {
    for (w::MemCfg cfg : {w::MemCfg::NonLinear32, w::MemCfg::Region64}) {
        w::CryptoRig rig;
        ASSERT_TRUE(rig.init(cfg));
        const CpuState init = rig.base();
        const XmmReg* x = init.sse.xmm;
        struct Case { std::vector<u8> code; int dst; XmmReg want; };
        const Case cases[] = {
            {{0x0F, 0x38, 0xCB, 0xC0}, 0, ref_rnds2(x[0], x[0], x[0])},
            {{0x0F, 0x38, 0xCB, 0xD2}, 2, ref_rnds2(x[2], x[2], x[0])},
            {{0x0F, 0x38, 0xCB, 0xC8}, 1, ref_rnds2(x[1], x[0], x[0])},
        };
        for (const Case& c : cases) {
            SCOPED_TRACE(::testing::Message() << w::cfg_name(cfg) << " dst=" << c.dst);
            const w::JitRun r = w::expect_parity(rig, c.code, init);
            EXPECT_EQ(r.cpu.sse.xmm[c.dst].u64_[0], c.want.u64_[0]);
            EXPECT_EQ(r.cpu.sse.xmm[c.dst].u64_[1], c.want.u64_[1]);
            EXPECT_FALSE(w::has_sse_op_call(r.words));
        }
    }
}

TEST(JitSha256Ws3Test, MemoryFormsDirectInEveryConfig) {
    for (w::MemCfg cfg : w::kAllCfgs) {
        w::CryptoRig rig;
        if (!rig.init(cfg)) {
            ADD_FAILURE() << "linear reservation unavailable: " << w::cfg_name(cfg);
            continue;
        }
        for (u8 opcode : {u8{0xCB}, kMsg1Opcode, kMsg2Opcode}) {
            u64 seed = 0xABCDEF01ULL + opcode;
            for (int v = 0; v < 8; ++v) {
                SCOPED_TRACE(::testing::Message() << w::cfg_name(cfg) << " op="
                             << std::hex << int(opcode) << " v=" << v);
                CpuState init = rig.base();
                const u64 lo = w::next_rand(seed), hi = w::next_rand(seed);
                w::put_m128(rig, w::kData, lo, hi);
                const w::JitRun r =
                    w::expect_parity(rig, {0x0F, 0x38, opcode, 0x0A}, init);
                if (opcode == 0xCB) {
                    XmmReg m{};
                    m.u64_[0] = lo; m.u64_[1] = hi;
                    const XmmReg want =
                        ref_rnds2(init.sse.xmm[1], m, init.sse.xmm[0]);
                    EXPECT_EQ(r.cpu.sse.xmm[1].u64_[0], want.u64_[0]);
                    EXPECT_EQ(r.cpu.sse.xmm[1].u64_[1], want.u64_[1]);
                }
                EXPECT_FALSE(w::has_sse_op_call(r.words));
            }
        }
    }
}

TEST(JitSha256Ws3Test, MemoryMisalignedRaisesGp) {
    for (w::MemCfg cfg : {w::MemCfg::NonLinear32, w::MemCfg::Region64}) {
        w::CryptoRig rig;
        ASSERT_TRUE(rig.init(cfg));
        for (u8 opcode : {u8{0xCB}, kMsg1Opcode, kMsg2Opcode}) {
            SCOPED_TRACE(::testing::Message() << w::cfg_name(cfg) << " " << int(opcode));
            CpuState init = rig.base();
            init.gpr.set64(w::kRdx, w::kData + 8);
            const std::vector<u8> code{0x0F, 0x38, opcode, 0x0A};
            CpuState interpreted = init;
            EXPECT_EQ(rig.interp(code, interpreted), ExecResult::Exception);
            w::expect_fault_untouched(rig.jit(code, init), init);
        }
    }
}

TEST(JitSha256Ws3Test, MemoryFaultLeavesDestination) {
    for (w::MemCfg cfg : {w::MemCfg::NonLinear32, w::MemCfg::Region64}) {
        w::CryptoRig rig;
        ASSERT_TRUE(rig.init(cfg));
        for (u8 opcode : {u8{0xCB}, kMsg1Opcode, kMsg2Opcode}) {
            SCOPED_TRACE(::testing::Message() << w::cfg_name(cfg) << " " << int(opcode));
            CpuState init = rig.base();
            init.gpr.set64(w::kRdx, w::kUnmapped);
            w::expect_fault_untouched(
                rig.jit({0x0F, 0x38, opcode, 0x0A}, init), init);
        }
    }
}

TEST(JitSha256Ws3Test, Ws3KillSwitchRestoresRnds2Helper) {
    w::ScopedEnv off("VITREO_WS3", "0");
    w::CryptoRig rig;
    ASSERT_TRUE(rig.init(w::MemCfg::NonLinear32));
    const w::JitRun reg = w::expect_parity(rig, {0x0F, 0x38, 0xCB, 0xCA}, rig.base());
    EXPECT_TRUE(w::has_sse_op_call(reg.words));
    w::put_m128(rig, w::kData, 0x0123456789ABCDEFULL, 0x1122334455667788ULL);
    const w::JitRun mem =
        w::expect_parity(rig, {0x0F, 0x38, kMsg1Opcode, 0x0A}, rig.base());
    EXPECT_TRUE(w::has_sse_op_call(mem.words));
}

TEST(JitSha256Ws3Test, Fips180AbcDigestThroughJit) {
    w::CryptoRig rig;
    ASSERT_TRUE(rig.init(w::MemCfg::NonLinear32));
    u32 wv[64] = {0x61626380u};  // "abc" padded, FIPS 180-4 §5.1.1
    wv[15] = 24;
    for (int t = 16; t < 64; ++t) {
        wv[t] = sigma1_ref(wv[t - 2]) + wv[t - 7] + sigma0_ref(wv[t - 15]) +
                wv[t - 16];
    }
    const u32 h0[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                       0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    // Intel arrangement: xmm1 = ABEF, xmm2 = CDGH (A / C at [127:96]).
    XmmReg abef{}, cdgh{};
    abef.u32_[3] = h0[0]; abef.u32_[2] = h0[1]; abef.u32_[1] = h0[4]; abef.u32_[0] = h0[5];
    cdgh.u32_[3] = h0[2]; cdgh.u32_[2] = h0[3]; cdgh.u32_[1] = h0[6]; cdgh.u32_[0] = h0[7];
    // SHA256RNDS2 xmm2,xmm1 ; PSHUFD xmm0,xmm0,0x0E ; SHA256RNDS2 xmm1,xmm2
    const std::vector<u8> code{0x0F, 0x38, 0xCB, 0xD1, 0x66, 0x0F, 0x70, 0xC0,
                               0x0E, 0x0F, 0x38, 0xCB, 0xCA};
    bool saw_helper = false;
    for (int i = 0; i < 16; ++i) {
        CpuState cpu = rig.base();
        for (int j = 0; j < 4; ++j) {
            cpu.sse.xmm[0].u32_[j] = wv[4 * i + j] + kSha256K[4 * i + j];
        }
        cpu.sse.xmm[1] = abef;
        cpu.sse.xmm[2] = cdgh;
        const w::JitRun r = rig.jit(code, cpu);
        ASSERT_TRUE(r.compiled);
        ASSERT_EQ(r.result, jit::JitExecResult::Continue);
        saw_helper |= w::has_sse_op_call(r.words);
        abef = r.cpu.sse.xmm[1];
        cdgh = r.cpu.sse.xmm[2];
    }
    const u32 digest[8] = {
        abef.u32_[3] + h0[0], abef.u32_[2] + h0[1], cdgh.u32_[3] + h0[2],
        cdgh.u32_[2] + h0[3], abef.u32_[1] + h0[4], abef.u32_[0] + h0[5],
        cdgh.u32_[1] + h0[6], cdgh.u32_[0] + h0[7]};
    const u32 expected[8] = {0xba7816bfu, 0x8f01cfeau, 0x414140deu, 0x5dae2223u,
                             0xb00361a3u, 0x96177a9cu, 0xb410ff61u, 0xf20015adu};
    for (int i = 0; i < 8; ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(digest[i], expected[i]);
    }
    EXPECT_FALSE(saw_helper);
}

} // namespace
