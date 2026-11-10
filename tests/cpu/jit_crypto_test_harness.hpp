// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// WS3 crypto JIT test harness: one instruction block run through the
// interpreter and the JIT (delegation off) in each memory configuration, with
// the emitted ARM64 words for code-shape assertions.

#pragma once

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/jit/jit_helpers.hpp"
#include "core/cpu/jit/stencils.hpp"

namespace ws3test {

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

inline constexpr GuestAddr kCode = 0x1000;
inline constexpr GuestAddr kData = 0x3000;      // 16-aligned, mapped
inline constexpr GuestAddr kUnmapped = 0x20000000;
inline constexpr u64 kFlags = 0x8D7u;
inline constexpr u8 kRdx = static_cast<u8>(RegIndex::RDX);
inline constexpr u8 kRsp = static_cast<u8>(RegIndex::RSP);

class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : m_name(name) {
        if (const char* prev = std::getenv(name)) m_prev = prev;
        if (value) setenv(name, value, 1); else unsetenv(name);
    }
    ~ScopedEnv() {
        if (m_prev) setenv(m_name.c_str(), m_prev->c_str(), 1);
        else unsetenv(m_name.c_str());
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;
private:
    std::string m_name;
    std::optional<std::string> m_prev;
};

/// LDR X16, [X20, #JIT_CTX_OFF_SSE_OP] -- the jit_sse_op dispatch load of
/// EMIT_SSE_OP_CALL. Asserted on instead of BLR X16 because the checked
/// memory slow paths legitimately BLR the READ64 helper.
inline bool has_sse_op_call(const std::vector<u32>& words) {
    const u32 needle = arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_SSE_OP >> 3);
    for (u32 w : words) if (w == needle) return true;
    return false;
}

/// Any ARM FEAT_AES instruction (AESE/AESD/AESMC/AESIMC).
inline bool has_arm_aes(const std::vector<u32>& words) {
    for (u32 w : words) {
        const u32 b = w & 0xFFFFFC00u;
        if (b == 0x4E284800u || b == 0x4E285800u || b == 0x4E286800u ||
            b == 0x4E287800u) return true;
    }
    return false;
}

enum class MemCfg { NonLinear32, Linear32, Region64, Linear64 };

inline bool cfg_bits64(MemCfg c) {
    return c == MemCfg::Region64 || c == MemCfg::Linear64;
}
inline bool cfg_linear(MemCfg c) {
    return c == MemCfg::Linear32 || c == MemCfg::Linear64;
}
inline const char* cfg_name(MemCfg c) {
    switch (c) {
    case MemCfg::NonLinear32: return "nonlinear32";
    case MemCfg::Linear32: return "linear32";
    case MemCfg::Region64: return "region64";
    case MemCfg::Linear64: return "linear64";
    }
    return "?";
}
inline constexpr std::array<MemCfg, 4> kAllCfgs{
    MemCfg::NonLinear32, MemCfg::Linear32, MemCfg::Region64, MemCfg::Linear64};

struct JitRun {
    CpuState cpu;
    std::vector<u32> words;
    bool compiled{false};
    JitExecResult result{JitExecResult::Continue};
};

/// xorshift64, for reproducible seeded vectors.
inline u64 next_rand(u64& s) {
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;
    return s;
}

inline void set_xmm(CpuState& cpu, int i, u64 lo, u64 hi) {
    cpu.sse.xmm[i].u64_[0] = lo;
    cpu.sse.xmm[i].u64_[1] = hi;
}

/// One GuestMemory in one configuration plus interpreter/JIT runners.
class CryptoRig {
public:
    /// False when the configuration's linear reservation is unavailable.
    bool init(MemCfg cfg) {
        m_cfg = cfg;
        m_memory = std::make_unique<GuestMemory>();
        if (cfg_linear(cfg) &&
            !m_memory->enable_linear_reservation(usize{1} << 32)) {
            return false;
        }
        return static_cast<bool>(
            m_memory->map_region(kCode, 0x10000, MemoryProtection::ReadWrite));
    }

    GuestMemory& mem() { return *m_memory; }
    bool bits64() const { return cfg_bits64(m_cfg); }

    /// Mode, flags, RIP, stack, RDX = kData, and a seeded pattern in every
    /// xmm and ymm_high so stray writes are visible.
    CpuState base() const {
        CpuState cpu;
        if (bits64()) {
            cpu.init_long_mode();
            cpu.gpr.set64(kRdx, kData);
            cpu.gpr.set64(kRsp, 0x10000);
        } else {
            cpu.init_protected_mode();
            cpu.gpr.set32(kRdx, static_cast<u32>(kData));
            cpu.gpr.set32(kRsp, 0x10000);
        }
        cpu.flags.rflags = kFlags;
        cpu.rip = kCode;
        u64 s = 0xC0FFEE1234567ULL;
        for (int i = 0; i < 16; ++i) {
            set_xmm(cpu, i, next_rand(s), next_rand(s));
            cpu.avx.ymm_high[i].u64_[0] = next_rand(s);
            cpu.avx.ymm_high[i].u64_[1] = next_rand(s);
        }
        return cpu;
    }

    BasicBlock decode(const std::vector<u8>& code) {
        BasicBlock block;
        block.start_addr = kCode;
        block.is_64bit = bits64();
        MemoryByteReader reader(code.data(), code.size());
        while (reader.remaining() > 0) {
            DecodedInstruction in;
            const DecodeError e = decode_instruction(
                reader, bits64() ? DecodeMode::Mode64 : DecodeMode::Mode32, in);
            EXPECT_EQ(e, DecodeError::Success);
            if (e != DecodeError::Success) break;
            block.instructions.push_back(in);
        }
        block.byte_length = static_cast<u32>(reader.position());
        block.instruction_count = static_cast<u32>(block.instructions.size());
        return block;
    }

    ExecResult interp(const std::vector<u8>& code, CpuState& cpu,
                      u32 count = 1) {
        EXPECT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
        Interpreter interpreter(cpu, *m_memory);
        return interpreter.run(count);
    }

    /// Compile and run once. Delegation off unless `allow_delegate`.
    JitRun jit(const std::vector<u8>& code, const CpuState& init,
               bool allow_delegate = false) {
        EXPECT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
        JitRun r;
        r.cpu = init;
        BasicBlock block = decode(code);
        std::optional<ScopedEnv> nd;
        if (!allow_delegate) nd.emplace("VITREO_JIT_NO_DELEGATE", "1");
        JitCompiler jitc(*m_memory);
        CompiledBlock* compiled = jitc.compile(block);
        if (!compiled) return r;
        r.compiled = true;
        const auto* bytes = static_cast<const u8*>(compiled->code);
        for (u32 off = 0; off + 4 <= compiled->code_size; off += 4) {
            u32 w = 0;
            std::memcpy(&w, bytes + off, 4);
            r.words.push_back(w);
        }
        r.result = jitc.execute(*compiled, r.cpu);
        return r;
    }

private:
    MemCfg m_cfg{MemCfg::NonLinear32};
    std::unique_ptr<GuestMemory> m_memory;
};

inline void expect_vec_state_equal(const CpuState& a, const CpuState& b) {
    for (int i = 0; i < 16; ++i) {
        SCOPED_TRACE(::testing::Message() << "xmm" << i);
        EXPECT_EQ(a.sse.xmm[i].u64_[0], b.sse.xmm[i].u64_[0]);
        EXPECT_EQ(a.sse.xmm[i].u64_[1], b.sse.xmm[i].u64_[1]);
        EXPECT_EQ(a.avx.ymm_high[i].u64_[0], b.avx.ymm_high[i].u64_[0]);
        EXPECT_EQ(a.avx.ymm_high[i].u64_[1], b.avx.ymm_high[i].u64_[1]);
    }
    EXPECT_EQ(a.flags.rflags, kFlags);
    EXPECT_EQ(b.flags.rflags, kFlags);
    EXPECT_EQ(a.rip, b.rip);
}

/// Interpreter vs JIT on `code` from `init`; returns the JIT run.
inline JitRun expect_parity(CryptoRig& rig, const std::vector<u8>& code,
                            const CpuState& init) {
    CpuState interpreted = init;
    EXPECT_EQ(rig.interp(code, interpreted), ExecResult::Continue);
    JitRun r = rig.jit(code, init);
    EXPECT_TRUE(r.compiled);
    EXPECT_EQ(r.result, JitExecResult::Continue);
    EXPECT_FALSE(r.cpu.pending_raise.active);
    expect_vec_state_equal(interpreted, r.cpu);
    return r;
}

/// The JIT run raised an access violation and changed no vector register.
inline void expect_fault_untouched(const JitRun& r, const CpuState& init) {
    EXPECT_TRUE(r.compiled);
    ASSERT_TRUE(r.cpu.pending_raise.active);
    EXPECT_EQ(r.cpu.pending_raise.code, 0xC0000005u);
    for (int i = 0; i < 16; ++i) {
        SCOPED_TRACE(::testing::Message() << "xmm" << i);
        EXPECT_EQ(r.cpu.sse.xmm[i].u64_[0], init.sse.xmm[i].u64_[0]);
        EXPECT_EQ(r.cpu.sse.xmm[i].u64_[1], init.sse.xmm[i].u64_[1]);
        EXPECT_EQ(r.cpu.avx.ymm_high[i].u64_[0], init.avx.ymm_high[i].u64_[0]);
        EXPECT_EQ(r.cpu.avx.ymm_high[i].u64_[1], init.avx.ymm_high[i].u64_[1]);
    }
    EXPECT_EQ(r.cpu.flags.rflags, kFlags);
}

/// Writes 16 bytes of `cpu.sse.xmm[src]` to kData (the m128 operand).
inline void put_m128(CryptoRig& rig, GuestAddr at, u64 lo, u64 hi) {
    ASSERT_TRUE(rig.mem().write_u64(at, lo));
    ASSERT_TRUE(rig.mem().write_u64(at + 8, hi));
}

} // namespace ws3test
