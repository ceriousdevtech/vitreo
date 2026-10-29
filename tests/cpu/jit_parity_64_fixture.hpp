// SPDX-License-Identifier: LGPL-2.1-only
// JitParity64Test, the fixture shared by test_jit_parity_64.cpp and the
// test_jit_parity_64_NN_*.cpp parts (64-bit long-mode parity, CPU-048).
// The parts are numbered because gtest runs a suite's tests in (file, line)
// order and some of these tests depend on process-global JIT and
// perf-window state: the numbers keep the original single-file run order.
#pragma once

#include <gtest/gtest.h>
#include <set>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include "core/cpu/jit/jit_compiler.hpp"
#include "jit_dispatch_execute.hpp"
#include "core/cpu/jit/jit_helpers.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/jit/linear_fault.hpp"
#include "core/cpu/jit/stencils.hpp"
#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include "runtime/execution/block_decoder.hpp"
#include "core/cpu/seh/seh32.hpp"
#include <vitreo/diagnostics.hpp>

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

constexpr u8 RAX = static_cast<u8>(RegIndex::RAX);
constexpr u8 RCX = static_cast<u8>(RegIndex::RCX);
constexpr u8 RDX = static_cast<u8>(RegIndex::RDX);
constexpr u8 RBX = static_cast<u8>(RegIndex::RBX);
constexpr u8 RSP = static_cast<u8>(RegIndex::RSP);
constexpr u8 RBP = static_cast<u8>(RegIndex::RBP);
constexpr u8 RSI = static_cast<u8>(RegIndex::RSI);
constexpr u8 RDI = static_cast<u8>(RegIndex::RDI);
constexpr u8 R8  = static_cast<u8>(RegIndex::R8);
constexpr u8 R9  = static_cast<u8>(RegIndex::R9);
constexpr u8 R10 = static_cast<u8>(RegIndex::R10);
constexpr u8 R11 = static_cast<u8>(RegIndex::R11);
constexpr u8 R15 = static_cast<u8>(RegIndex::R15);

inline const char* gpr_name64(int i) {
    static const char* names[] = {
        "RAX","RCX","RDX","RBX","RSP","RBP","RSI","RDI",
        "R8","R9","R10","R11","R12","R13","R14","R15"
    };
    return (i >= 0 && i < 16) ? names[i] : "???";
}

class ScopedEnvironmentVariable {
public:
    ScopedEnvironmentVariable(const char* name, const char* value)
        : m_name(name) {
        if (const char* previous = std::getenv(name)) {
            m_previous = previous;
        }
        if (value != nullptr) setenv(name, value, 1);
        else unsetenv(name);
    }

    ~ScopedEnvironmentVariable() {
        if (m_previous.has_value()) {
            setenv(m_name.c_str(), m_previous->c_str(), 1);
        } else {
            unsetenv(m_name.c_str());
        }
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

private:
    std::string m_name;
    std::optional<std::string> m_previous;
};

/// The environment as the PROCESS started, before any suite's own setup or
/// teardown could change it: a ctest rerun that asks for the linear model or
/// the JIT view (tests/CMakeLists.txt) must get it in every JitParity64Test,
/// whatever ran earlier in the same binary.
inline const bool kParityLinear64AtStart = [] {
    const char* v = std::getenv("VITREO_TEST_PARITY_LINEAR64");
    return v != nullptr && v[0] == '1';
}();
inline const bool kJitViewAtStart = [] {
    const char* v = std::getenv("VITREO_JIT_VIEW");
    return v != nullptr && v[0] == '1';
}();

class JitParity64Test : public ::testing::Test {
protected:
    void SetUp() override {
        if (kJitViewAtStart) {
            const char* v = std::getenv("VITREO_JIT_VIEW");
            ASSERT_TRUE(v != nullptr && v[0] == '1')
                << "VITREO_JIT_VIEW was set when the process started and an "
                   "earlier suite cleared it: this test would silently run "
                   "without the JIT view";
        }
        m_memory = std::make_unique<GuestMemory>();
        // VITREO_TEST_PARITY_LINEAR64=1 (a ctest rerun, tests/CMakeLists.txt):
        // the same interpreter-oracle tests on the hardware-protected linear
        // model -- and with VITREO_JIT_VIEW=1 through the JIT view, which is
        // where translated code must stay exact.
        if (const char* v = std::getenv("VITREO_TEST_PARITY_LINEAR64");
            v != nullptr && v[0] == '1') {
            // 64 GiB: the high-lane tests cross 0x2_0000_0000 and must stay
            // inside the window (a region straddling the window end is a
            // separate, recorded defect of the linear model).
            constexpr usize kParityReservation = usize{1} << 36;
            ASSERT_TRUE(install_linear_fault_handler());
            ASSERT_TRUE(m_memory->enable_linear_reservation(kParityReservation));
            ASSERT_TRUE(m_memory->enable_linear_hardware_protection());
            if (const char* view = std::getenv("VITREO_JIT_VIEW");
                view != nullptr && view[0] == '1') {
                ASSERT_TRUE(m_memory->enable_linear_jit_view());
            }
            ASSERT_TRUE(m_registration.acquire(m_memory->linear_base(),
                                               kParityReservation));
            if (m_memory->linear_jit_view_base() != nullptr) {
                ASSERT_TRUE(m_registration.acquire_jit_view(
                    m_memory->linear_jit_view_base(), kParityReservation));
            }
            if (kJitViewAtStart) {
                ASSERT_NE(m_memory->linear_jit_view_base(), nullptr)
                    << "the JIT view was requested but is not engaged";
            }
        } else {
            ASSERT_FALSE(kParityLinear64AtStart)
                << "VITREO_TEST_PARITY_LINEAR64 was cleared by an earlier suite";
        }
        auto map_result = m_memory->map_region(0x1000, 0x10000,
                                                MemoryProtection::ReadWrite);
        ASSERT_TRUE(map_result);
        m_jit = std::make_unique<JitCompiler>(*m_memory);
    }

    BasicBlock build_block(const u8* code, usize len) {
        BasicBlock block;
        block.start_addr = 0x1000;
        block.is_64bit = true;

        MemoryByteReader reader(code, len);
        while (reader.remaining() > 0) {
            DecodedInstruction inst;
            DecodeError err = decode_instruction(reader, DecodeMode::Mode64, inst);
            if (err != DecodeError::Success) break;
            block.instructions.push_back(inst);
        }

        block.byte_length = static_cast<u32>(reader.position());
        block.instruction_count =
            static_cast<u32>(block.instructions.size());
        return block;
    }

    void init_cpu(CpuState& cpu) {
        cpu.init_long_mode();
        cpu.gpr.set64(RSP, 0x10000);
        cpu.rip = 0x1000;
    }

    void check_parity(const u8* code, usize len,
                      std::function<void(CpuState&)> setup = {},
                      bool check_flags = true,
                      bool check_xmm = false,
                      bool require_jit = false,
                      bool check_af = false) {
        ASSERT_TRUE(m_memory->write_bytes(0x1000, code, len));
        // Interpreter and JIT must start from identical writable guest
        // memory. Without this snapshot, a destination-memory instruction is
        // applied once by the interpreter and then a second time by the JIT,
        // which both corrupts parity and can hide a missing JIT store.
        std::vector<std::vector<u8>> snapshots;
        snapshots.emplace_back(0x10000);
        ASSERT_TRUE(m_memory->read_bytes(
            0x1000, snapshots[0].data(), snapshots[0].size()));
        for (const auto& region : m_tracked_regions) {
            snapshots.emplace_back(region.second);
            ASSERT_TRUE(m_memory->read_bytes(region.first,
                                             snapshots.back().data(),
                                             snapshots.back().size()));
        }

        m_last_parity_ok = false;
        BasicBlock block = build_block(code, len);
        ASSERT_GT(block.instruction_count, 0u) << "No instructions decoded";

        // ---- Interpreter ----
        CpuState icpu;
        init_cpu(icpu);
        if (setup) setup(icpu);

        {
            Interpreter interp(icpu, *m_memory);
            interp.run(block.instruction_count);
        }
        icpu.materialize_lazy_flags();

        // The interpreter is the semantic reference for guest MEMORY too, not
        // just registers. A REP string lowering that steps the wrong way, or
        // writes one element too many, leaves every register right and the
        // buffer wrong — invisible to a register-only comparison.
        std::vector<std::vector<u8>> interp_memory;
        if (m_compare_memory) {
            interp_memory.emplace_back(snapshots[0].size());
            ASSERT_TRUE(m_memory->read_bytes(
                0x1000, interp_memory[0].data(), interp_memory[0].size()));
            for (const auto& region : m_tracked_regions) {
                interp_memory.emplace_back(region.second);
                ASSERT_TRUE(m_memory->read_bytes(region.first,
                                                 interp_memory.back().data(),
                                                 interp_memory.back().size()));
            }
        }

        // ---- JIT ----
        ASSERT_TRUE(m_memory->write_bytes(
            0x1000, snapshots[0].data(), snapshots[0].size()));
        for (usize i = 0; i < m_tracked_regions.size(); ++i) {
            ASSERT_TRUE(m_memory->write_bytes(m_tracked_regions[i].first,
                                              snapshots[i + 1].data(),
                                              snapshots[i + 1].size()));
        }
        CpuState jcpu;
        init_cpu(jcpu);
        if (setup) setup(jcpu);

        CompiledBlock* cb = m_jit->compile(block);
        if (!cb) {
            if (require_jit) {
                ADD_FAILURE() << "Expected a fully native 64-bit JIT block";
                return;
            }
            GTEST_SKIP() << "JIT does not support 64-bit opcode";
        }
        test::execute_like_dispatcher(*m_jit, *cb, jcpu);
        jcpu.materialize_lazy_flags();
        m_last_interp_cpu = icpu;
        m_last_jit_cpu = jcpu;

        if (m_compare_memory) {
            auto compare_region = [&](GuestAddr addr,
                                      const std::vector<u8>& expected) {
                std::vector<u8> actual(expected.size());
                ASSERT_TRUE(m_memory->read_bytes(addr, actual.data(),
                                                 actual.size()));
                for (usize i = 0; i < expected.size(); ++i) {
                    ASSERT_EQ(expected[i], actual[i])
                        << "guest memory diverged at 0x" << std::hex
                        << (addr + i) << ": interp=0x"
                        << static_cast<unsigned>(expected[i]) << " jit=0x"
                        << static_cast<unsigned>(actual[i]);
                }
            };
            compare_region(0x1000, interp_memory[0]);
            for (usize i = 0; i < m_tracked_regions.size(); ++i) {
                compare_region(m_tracked_regions[i].first,
                               interp_memory[i + 1]);
            }
        }

        // ---- Compare GPRs (all 16 in 64-bit mode) ----
        for (int i = 0; i < 16; i++) {
            EXPECT_EQ(icpu.gpr.get64(i), jcpu.gpr.get64(i))
                << gpr_name64(i) << " diverged: interp=0x" << std::hex
                << icpu.gpr.get64(i) << " jit=0x" << jcpu.gpr.get64(i);
        }

        EXPECT_EQ(icpu.rip, jcpu.rip)
            << "RIP diverged: interp=0x" << std::hex << icpu.rip
            << " jit=0x" << jcpu.rip;

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

        if (check_xmm) {
            for (int r = 0; r < 16; r++) {
                for (int b = 0; b < 16; b++) {
                    EXPECT_EQ(icpu.sse.xmm[r].u8_[b], jcpu.sse.xmm[r].u8_[b])
                        << "XMM" << r << "[" << b << "] diverged";
                    // YMM[255:128] must be compared too. Every VEX.128 op is
                    // specified to zero the upper half (Intel SDM Vol. 2), and
                    // Vitreo has a recorded bug where VEX ops were lowered as
                    // legacy 2-operand SSE and left it stale. Comparing only
                    // sse.xmm made that class of divergence invisible to every
                    // check_xmm test in this file.
                    EXPECT_EQ(icpu.avx.ymm_high[r].u8_[b],
                              jcpu.avx.ymm_high[r].u8_[b])
                        << "YMM" << r << "[" << (16 + b) << "] diverged";
                }
            }
            // A lowering must reproduce MXCSR's sticky exception bits too, not
            // just the values (recorded: jit_lowering_must_match_mxcsr). DIVPS
            // by zero sets ZE, and an inexact quotient sets PE; neither is
            // visible in any register.
            EXPECT_EQ(icpu.sse.mxcsr, jcpu.sse.mxcsr)
                << "MXCSR diverged: interp=0x" << std::hex << icpu.sse.mxcsr
                << " jit=0x" << jcpu.sse.mxcsr;
        }
        m_last_parity_ok = true;
    }

    void check_parity(std::initializer_list<u8> code,
                      std::function<void(CpuState&)> setup = {},
                      bool check_flags = true,
                      bool check_xmm = false,
                      bool require_jit = false,
                      bool check_af = false) {
        std::vector<u8> v(code);
        check_parity(v.data(), v.size(), setup, check_flags, check_xmm,
                     require_jit, check_af);
    }

    void check_native_parity(
        std::initializer_list<u8> code,
        std::function<void(CpuState&)> setup = {},
        bool check_flags = true,
        bool check_xmm = false,
        bool check_af = false) {
        ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
        std::vector<u8> v(code);
        check_parity(v.data(), v.size(), setup, check_flags, check_xmm,
                     /*require_jit=*/true, check_af);
    }

    // Map a writable data region and include it in the pre-run snapshot. A
    // destination-memory instruction outside the low snapshot would otherwise
    // let the JIT start from bytes the interpreter already modified.
    void map_tracked_region(GuestAddr addr, usize size) {
        ASSERT_TRUE(m_memory->map_region(addr, size,
                                         MemoryProtection::ReadWrite));
        m_tracked_regions.emplace_back(addr, size);
    }

    std::unique_ptr<GuestMemory> m_memory;
    std::unique_ptr<JitCompiler> m_jit;
    // Declared after both so it unregisters the fault windows first.
    LinearReservationRegistration m_registration;
    std::vector<std::pair<GuestAddr, usize>> m_tracked_regions;
    // Opt-in: compare the post-run guest bytes of the snapshotted regions
    // against the interpreter's, not just the registers.
    bool m_compare_memory{false};
    // Post-run state of the last check_parity run, so a test can assert an
    // absolute contract (e.g. "REP with count 0 does not touch RSI/RDI")
    // rather than only interpreter agreement.
    CpuState m_last_interp_cpu{};
    CpuState m_last_jit_cpu{};
    // False when check_parity bailed out (no JIT block, decode failure), so a
    // caller does not go on to assert against a stale m_last_jit_cpu.
    bool m_last_parity_ok{false};
};

// Nonzero in BOTH lanes of every destination YMM used below. A VEX.128 form
// that fails to zero YMM[255:128] leaves this pattern behind and diverges from
// the interpreter, which clears it.
inline void seed_ymm_dirty(CpuState& cpu, int reg) {
    for (int i = 0; i < 4; ++i) {
        cpu.sse.xmm[reg].u32_[i] = 0xA5A5A500u + static_cast<u32>(i);
        cpu.avx.ymm_high[reg].u32_[i] = 0x5A5A5A00u + static_cast<u32>(i);
    }
}
