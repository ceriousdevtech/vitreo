// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests (32-bit mode): shared fixture
//
// Runs the SAME x86 machine code through both the interpreter and the JIT,
// then diffs ALL CPU state (GPRs, flags, XMM, memory).  Any divergence is
// a potential source of the Steam.exe crash.

#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include "core/cpu/jit/jit_compiler.hpp"
#include "jit_dispatch_execute.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include "core/cpu/seh/seh32.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

// Register indices
constexpr u8 EAX = static_cast<u8>(RegIndex::RAX);
constexpr u8 ECX = static_cast<u8>(RegIndex::RCX);
constexpr u8 EDX = static_cast<u8>(RegIndex::RDX);
constexpr u8 EBX = static_cast<u8>(RegIndex::RBX);
constexpr u8 ESP = static_cast<u8>(RegIndex::RSP);
constexpr u8 EBP = static_cast<u8>(RegIndex::RBP);
constexpr u8 ESI = static_cast<u8>(RegIndex::RSI);
constexpr u8 EDI = static_cast<u8>(RegIndex::RDI);

inline const char* gpr_name(int i) {
    static const char* names[] = {"EAX","ECX","EDX","EBX","ESP","EBP","ESI","EDI"};
    return (i >= 0 && i < 8) ? names[i] : "???";
}

class ScopedEnvVar {
public:
    ScopedEnvVar(const char* name, const char* value)
        : m_name(name), m_had(false) {
        const char* prev = std::getenv(name);
        if (prev) {
            m_had = true;
            m_prev = prev;
        }
        setenv(name, value, 1);
    }
    ~ScopedEnvVar() {
        if (m_had) setenv(m_name, m_prev.c_str(), 1);
        else unsetenv(m_name);
    }
    ScopedEnvVar(const ScopedEnvVar&) = delete;
    ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;
private:
    const char* m_name;
    bool m_had;
    std::string m_prev;
};

class JitParityTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        ASSERT_TRUE(m_memory->map_region(0x1000, 0x10000,
                                         MemoryProtection::ReadWriteExecute).is_ok());
        m_jit = std::make_unique<JitCompiler>(*m_memory);
    }

    // Build a BasicBlock from raw machine code bytes
    BasicBlock build_block(const u8* code, usize len) {
        BasicBlock block;
        block.start_addr = 0x1000;

        MemoryByteReader reader(code, len);
        while (reader.remaining() > 0) {
            DecodedInstruction inst;
            DecodeError err = decode_instruction(reader, DecodeMode::Mode32, inst);
            if (err != DecodeError::Success) break;
            block.instructions.push_back(inst);
        }

        block.byte_length = static_cast<u32>(reader.position());
        block.instruction_count =
            static_cast<u32>(block.instructions.size());
        return block;
    }

    // Initialize a CpuState for 32-bit mode testing
    void init_cpu(CpuState& cpu) {
        cpu.init_protected_mode();
        cpu.gpr.set32(ESP, 0x10000);
        cpu.rip = 0x1000;
    }

    /// Run code through BOTH interpreter and JIT, compare all state.
    /// @param code        x86 machine code
    /// @param len         byte count
    /// @param setup       optional callback to set initial register values
    /// @param check_flags whether to compare flag bits (some instructions
    ///                    leave flags in an undefined state)
    /// @param check_xmm   check XMM 0-7 *and* YMM[255:128] 0-7 for SSE/AVX
    ///                    parity
    /// @param require_jit fail loudly (rather than silently accepting a
    ///                    truncated or delegating block) when the JIT did not
    ///                    produce a block. Pair with VITREO_JIT_NO_DELEGATE
    ///                    via check_native_parity to assert a *native*
    ///                    lowering rather than an interpreter delegate.
    void check_parity(const u8* code, usize len,
                      std::function<void(CpuState&)> setup = {},
                      bool check_flags = true,
                      bool check_xmm = false,
                      bool require_jit = false,
                      bool check_af = false) {
        ASSERT_TRUE(m_memory->write_bytes(0x1000, code, len));

        // Snapshot the mapped test region so interpreter and JIT both start
        // from identical memory state.
        std::vector<u8> initial_mem(0x10000);
        ASSERT_TRUE(m_memory->read_bytes(0x1000, initial_mem.data(), initial_mem.size()));

        // Decode once to build block (shared between both paths)
        BasicBlock block = build_block(code, len);
        ASSERT_GT(block.instruction_count, 0u) << "No instructions decoded";

        // ---- Interpreter path ----
        CpuState icpu;
        init_cpu(icpu);
        if (setup) setup(icpu);

        {
            Interpreter interp(icpu, *m_memory);
            auto result = interp.run(block.instruction_count);
            EXPECT_EQ(result, ExecResult::Continue) << "Interpreter failed";
        }
        icpu.materialize_lazy_flags();

        std::vector<u8> interp_mem(0x10000);
        ASSERT_TRUE(m_memory->read_bytes(0x1000, interp_mem.data(), interp_mem.size()));

        // Reset memory to pre-run snapshot before JIT execution.
        ASSERT_TRUE(m_memory->write_bytes(0x1000, initial_mem.data(), initial_mem.size()));

        // ---- JIT path ----
        CpuState jcpu;
        init_cpu(jcpu);
        if (setup) setup(jcpu);

        // Fresh compiler: a shared one would accumulate churn demotions after
        // enough recompiles of the same guest address across tests.
        JitCompiler jit(*m_memory);
        CompiledBlock* cb = jit.compile(block);
        if (require_jit) {
            ASSERT_NE(cb, nullptr)
                << "Expected a fully native 32-bit JIT block; the JIT declined "
                   "the opcode (with VITREO_JIT_NO_DELEGATE set, declining "
                   "means there is no native lowering for this form)";
        }
        ASSERT_NE(cb, nullptr) << "JIT compilation failed";
        auto jit_result = test::execute_like_dispatcher(jit, *cb, jcpu);
        EXPECT_EQ(jit_result, JitExecResult::Continue) << "JIT execution failed";
        jcpu.materialize_lazy_flags();

        std::vector<u8> jit_mem(0x10000);
        ASSERT_TRUE(m_memory->read_bytes(0x1000, jit_mem.data(), jit_mem.size()));

        // ---- Compare GPRs ----
        for (int i = 0; i < 8; i++) {
            EXPECT_EQ(icpu.gpr.get32(i), jcpu.gpr.get32(i))
                << gpr_name(i) << " diverged: interp=0x" << std::hex
                << icpu.gpr.get32(i) << " jit=0x" << jcpu.gpr.get32(i);
        }

        // ---- Compare EIP ----
        EXPECT_EQ(static_cast<u32>(icpu.rip), static_cast<u32>(jcpu.rip))
            << "EIP diverged: interp=0x" << std::hex
            << static_cast<u32>(icpu.rip)
            << " jit=0x" << static_cast<u32>(jcpu.rip);

        // ---- Compare flags ----
        if (check_flags) {
            EXPECT_EQ(icpu.flags.cf(), jcpu.flags.cf()) << "CF diverged";
            EXPECT_EQ(icpu.flags.zf(), jcpu.flags.zf()) << "ZF diverged";
            EXPECT_EQ(icpu.flags.sf(), jcpu.flags.sf()) << "SF diverged";
            EXPECT_EQ(icpu.flags.of(), jcpu.flags.of()) << "OF diverged";
            EXPECT_EQ(icpu.flags.pf(), jcpu.flags.pf()) << "PF diverged";
            if (check_af) {
                EXPECT_EQ(icpu.flags.af(), jcpu.flags.af()) << "AF diverged";
            }
        }

        // ---- Compare XMM registers ----
        if (check_xmm) {
            for (int r = 0; r < 8; r++) {
                for (int b = 0; b < 16; b++) {
                    EXPECT_EQ(icpu.sse.xmm[r].u8_[b], jcpu.sse.xmm[r].u8_[b])
                        << "XMM" << r << "[" << b << "] diverged";
                    // YMM[255:128] must be compared too. VEX.256 is legal in
                    // compatibility mode and BoringSSL emits it from 32-bit
                    // code, and every VEX.128 form is specified to ZERO the
                    // upper half (Intel SDM Vol. 2, 2.3.5). Comparing only
                    // sse.xmm made a stale-or-unwritten high lane invisible to
                    // every check_xmm test in this file.
                    EXPECT_EQ(icpu.avx.ymm_high[r].u8_[b],
                              jcpu.avx.ymm_high[r].u8_[b])
                        << "YMM" << r << "[" << (16 + b) << "] diverged";
                }
            }
        }

        // ---- Compare memory (full mapped test region) ----
        for (usize i = 0; i < interp_mem.size(); ++i) {
            EXPECT_EQ(interp_mem[i], jit_mem[i])
                << "Memory diverged at 0x" << std::hex << (0x1000u + static_cast<u32>(i));
        }
    }

    /// Overload with initializer_list for convenience
    void check_parity(std::initializer_list<u8> code,
                      std::function<void(CpuState&)> setup = {},
                      bool check_flags = true,
                      bool check_xmm = false,
                      bool require_jit = false) {
        std::vector<u8> v(code);
        check_parity(v.data(), v.size(), setup, check_flags, check_xmm,
                     require_jit);
    }

    /// Same as check_parity, but with the interpreter delegate disabled for
    /// the duration of the compile. The JIT must lower the code natively or
    /// the test fails -- a lowering that quietly falls back to the
    /// interpreter would otherwise pass every parity assertion.
    void check_native_parity(const u8* code, usize len,
                             std::function<void(CpuState&)> setup = {},
                             bool check_flags = true,
                             bool check_xmm = false) {
        ScopedEnvVar no_delegate("VITREO_JIT_NO_DELEGATE", "1");
        check_parity(code, len, setup, check_flags, check_xmm,
                     /*require_jit=*/true);
    }

    void check_native_parity(std::initializer_list<u8> code,
                             std::function<void(CpuState&)> setup = {},
                             bool check_flags = true,
                             bool check_xmm = false) {
        std::vector<u8> v(code);
        check_native_parity(v.data(), v.size(), setup, check_flags, check_xmm);
    }

    std::unique_ptr<GuestMemory> m_memory;
    std::unique_ptr<JitCompiler> m_jit;
};

// =============================================================================
// x87 result parity
// =============================================================================
//
// check_parity compares GPRs, RIP and flags but never the x87 stack, so a
// lowering that computes the wrong float passes every existing parity test.
// SimCity 4 renders terrain with fld/fmul/faddp chains, and a wrong result
// there is invisible to the suite while being glaringly visible on screen.
//
// Intel SDM Vol. 1 §8.1.5.2: the PC field (control word bits 9:8) selects the
// significand width used for arithmetic -- 00B single (24 bits), 10B double
// (53 bits), 11B double-extended (64 bits). Vitreo's interpreter is the
// semantic reference (FpuState::round_result_to_precision), so the JIT must
// agree with it at every PC setting.
class JitFpuParityTest : public JitParityTest {
protected:
    /// Runs `code` through ONE engine and returns ST0, so a caller can
    /// inspect guest memory between the two runs.
    double run_engine(const std::vector<u8>& code, u16 cw, bool use_jit) {
        EXPECT_TRUE(m_memory->write_bytes(0x1000, code.data(), code.size()));
        BasicBlock block = build_block(code.data(), code.size());
        EXPECT_GT(block.instruction_count, 0u);
        CpuState cpu;
        init_cpu(cpu);
        cpu.fpu.control_word = cw;
        if (use_jit) {
            JitCompiler jit(*m_memory);
            auto* compiled = jit.compile(block);
            EXPECT_NE(compiled, nullptr) << "JIT declined the block";
            if (compiled) {
                EXPECT_EQ(test::execute_like_dispatcher(jit, *compiled, cpu), JitExecResult::Continue);
            }
        } else {
            Interpreter interp(cpu, *m_memory);
            EXPECT_EQ(interp.run(block.instruction_count), ExecResult::Continue);
        }
        return cpu.fpu.get_st(0).to_double();
    }

    /// Like st0_both, but also compares `len` bytes written at `addr` -- the
    /// store forms put their result in guest memory, not on the stack.
    std::pair<double, double> st0_both_with_mem(const std::vector<u8>& code,
                                                u16 cw, u32 addr, u32 len) {
        std::vector<u8> before(len ? len : 1);
        if (len) EXPECT_TRUE(m_memory->read_bytes(addr, before.data(), len));
        const double interp_st0 = run_engine(code, cw, /*use_jit=*/false);
        std::vector<u8> interp_bytes(len ? len : 1);
        if (len) EXPECT_TRUE(m_memory->read_bytes(addr, interp_bytes.data(), len));
        if (len) EXPECT_TRUE(m_memory->write_bytes(addr, before.data(), len));
        const double jit_st0 = run_engine(code, cw, /*use_jit=*/true);
        std::vector<u8> jit_bytes(len ? len : 1);
        if (len) EXPECT_TRUE(m_memory->read_bytes(addr, jit_bytes.data(), len));
        if (len) EXPECT_EQ(interp_bytes, jit_bytes) << "stored bytes diverged";
        return {interp_st0, jit_st0};
    }

    /// Runs `code` through both engines and returns (interpreter, JIT) ST0.
    std::pair<double, double> st0_both(const std::vector<u8>& code,
                                       u16 control_word) {
        EXPECT_TRUE(m_memory->write_bytes(0x1000, code.data(), code.size()));
        BasicBlock block = build_block(code.data(), code.size());
        EXPECT_GT(block.instruction_count, 0u);

        CpuState icpu;
        init_cpu(icpu);
        icpu.fpu.control_word = control_word;
        {
            Interpreter interp(icpu, *m_memory);
            EXPECT_EQ(interp.run(block.instruction_count), ExecResult::Continue);
        }

        CpuState jcpu;
        init_cpu(jcpu);
        jcpu.fpu.control_word = control_word;
        {
            JitCompiler jit(*m_memory);
            auto* compiled = jit.compile(block);
            EXPECT_NE(compiled, nullptr) << "JIT declined the block";
            if (compiled) {
                EXPECT_EQ(test::execute_like_dispatcher(jit, *compiled, jcpu), JitExecResult::Continue);
            }
        }
        // A JIT block may legitimately stop early (an instruction it declines
        // emits an epilogue and returns Continue). Comparing ST0 then would be
        // comparing a partial run against a complete one, so surface it.
        EXPECT_EQ(static_cast<u32>(icpu.rip), static_cast<u32>(jcpu.rip))
            << "JIT stopped at a different instruction than the interpreter; "
               "ST0 comparison below is not like-for-like";
        return {icpu.fpu.get_st(0).to_double(), jcpu.fpu.get_st(0).to_double()};
    }
};
