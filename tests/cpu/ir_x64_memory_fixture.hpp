// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file ir_x64_memory_fixture.hpp
/// @brief IrEmit64Memory and IrEmit64NarrowAlu fixtures shared by the test_ir_x64_*.cpp suites.
///
/// IrEmit64NarrowAlu spans test_ir_x64_narrow_alu_<n>_*.cpp. gtest runs a
/// suite's tests sorted by (file, line), so the ordinal in those names keeps
/// the suite's original execution order.

#pragma once

// These tests execute emitted ARM64, so Intel hosts must not reference the
// native-only IR backend through this translation unit.
#if defined(__aarch64__)
#include <gtest/gtest.h>
#include <array>
#include <algorithm>
#include <memory>
#include <cstdlib>
#include <vector>
#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include "ir_tier1_first_compile.hpp"
#include "core/cpu/jit/linear_fault.hpp"
#include "core/cpu/jit/stencils.hpp"
#include "core/cpu/jit/jit_helpers.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include "core/memory/guest_memory.hpp"
#include "core/memory/write_watch_traps.hpp"

namespace vitreo::cpu::jit::test::ir_x64_memory {
inline constexpr GuestAddr kCode = 0x100010000ull;
inline constexpr GuestAddr kData = 0x200010000ull;
// A separate stack region for the M12 forms, far from kData so a
// mis-sized slot lands somewhere visible rather than in the operand.
inline constexpr GuestAddr kStack = 0x300010000ull;

class IrEmit64Memory : public ::testing::Test {
protected:
    vitreo::cpu::jit::test::IrTier1FirstCompile m_tier1_first_compile;
    void SetUp() override {
        if (!JitCompiler::ir_enabled_for(true))
            GTEST_SKIP() << "x64 IR explicitly disabled by policy";
        if (std::getenv("VITREO_TEST_IR_LINEAR64")) {
            constexpr usize reservation = usize{16} << 30;
            ASSERT_TRUE(install_linear_fault_handler());
            ASSERT_TRUE(memory.enable_linear_reservation(reservation));
            ASSERT_TRUE(memory.enable_linear_hardware_protection());
            ASSERT_TRUE(register_linear_reservation(memory.linear_base(), reservation));
        }
        ASSERT_TRUE(memory.map_region(kCode, 0x1000,
                                      MemoryProtection::ReadWriteExecute));
        jit = std::make_unique<JitCompiler>(memory);
    }
    void TearDown() override {
        jit.reset();
        if (memory.is_linear()) unregister_linear_reservation(memory.linear_base());
    }
    CompiledBlock* compile(const std::vector<u8>& bytes) {
        EXPECT_TRUE(memory.write_bytes(kCode, bytes.data(), bytes.size()));
        BasicBlock block;
        block.start_addr = kCode;
        block.is_64bit = true;
        MemoryByteReader reader(bytes.data(), bytes.size());
        while (reader.remaining()) {
            DecodedInstruction instruction;
            const auto result = decode_instruction(reader, DecodeMode::Mode64,
                                                    instruction);
            EXPECT_EQ(result, DecodeError::Success);
            if (result != DecodeError::Success) return nullptr;
            block.instructions.push_back(instruction);
        }
        block.byte_length = static_cast<u32>(bytes.size());
        block.instruction_count = static_cast<u32>(block.instructions.size());
        const auto lowered = jit->ir_build_block(block);
        EXPECT_TRUE(lowered.ok) << ir::decline_reason_name(lowered.reason);
        if (!lowered.ok) return nullptr;
        auto* native = jit->compile(block);
        EXPECT_NE(native, nullptr);
        EXPECT_TRUE(jit->ir_emit_active())
            << "must execute native IR; backend declined with "
            << ir::decline_reason_name(jit->ir_emit_decline_reason());
        return native;
    }
    CpuState seed() {
        CpuState state;
        state.init_long_mode();
        state.rip = kCode;
        state.flags.rflags = 0xAD7;
        state.gpr.rax = 0xFEDCBA9876543210ull;
        return state;
    }
    /// Whether the block addresses the invocation-local spill frame at all.
    ///
    /// Deliberately NOT asserted from the behavioural tests below: which
    /// storage a surviving value lands in is the allocator's choice and
    /// depends on the enabled passes -- a guest register the block never
    /// writes stays in its architectural pin, and a constant is recomputed.
    /// Only a value that must cross an access helper is guaranteed to need a
    /// slot, and one test asserts exactly that.
    [[nodiscard]] bool uses_spill_backing(const CompiledBlock& native) const {
        const auto* words = static_cast<const u32*>(native.code);
        const auto* end = words + native.code_size / sizeof(u32);
        // X20 is the context pin; X3 is the backend's spill-address scratch.
        const u32 load_frame = arm64::ldr_x_uoff(3, 20,
                                                JIT_CTX_OFF_IR_SPILL_FRAME >> 3);
        return std::find(words, end, load_frame) != end;
    }
    GuestMemory memory;
    std::unique_ptr<BlockCache> cache;
    std::unique_ptr<JitCompiler> jit;
};

inline void append_disp32(std::vector<u8>& bytes, i64 disp) {
    const u32 bits = static_cast<u32>(disp);
    for (u32 shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<u8>(bits >> shift));
}

// =============================================================================
// Long-mode narrow ALU (M11b)
// =============================================================================

// The whole accepted shape space, swept. Flags are the point: a narrow record
// masks its payloads to its own width, so a lowering that published the wrong
// width would agree on the RESULT and disagree only on CF/OF/SF/PF — which is
// why every case compares materialized RFLAGS as well as all sixteen GPRs and
// the scratch window.
class IrEmit64NarrowAlu : public IrEmit64Memory {
protected:
    void expect_alu_matches_interpreter(const std::vector<u8>& bytes,
                                        unsigned instructions = 1,
                                        u64 ignore_flags = 0) {
        SCOPED_TRACE(testing::Message() << bytes.size() << " bytes");
        jit = std::make_unique<JitCompiler>(memory);
        auto* native = compile(bytes);
        ASSERT_NE(native, nullptr);
        for (const u64 seed_value : {0ull, 1ull, 0x7F7F7F7F7F7F7F7Full,
                                     0x8080808080808080ull,
                                     0xFFFFFFFFFFFFFFFFull,
                                     0x0123456789ABCDEFull}) {
            auto oracle = seed();
            for (u8 index = 0; index < 16; ++index)
                oracle.gpr.set64(index, seed_value ^ (0x0F0F0F0F0F0F0F0Full * index));
            oracle.gpr.set64(0, kData);
            oracle.flags.rflags = 0xAD7;
            auto state = oracle;
            ASSERT_TRUE(memory.write_u64(kData, seed_value).is_ok());
            {
                Interpreter interpreter(oracle, memory);
                for (unsigned step = 0; step < instructions; ++step)
                    ASSERT_EQ(interpreter.step(), ExecResult::Continue);
            }
            const auto oracle_memory = memory.read_u64(kData);
            ASSERT_TRUE(memory.write_u64(kData, seed_value).is_ok());
            ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
            EXPECT_EQ(memory.read_u64(kData).value_or(1), oracle_memory.value_or(2))
                << "guest memory diverged, seed=" << std::hex << seed_value;
            for (u8 index = 0; index < 16; ++index)
                EXPECT_EQ(state.gpr.get64(index), oracle.gpr.get64(index))
                    << "GPR " << unsigned(index) << " seed=" << std::hex << seed_value;
            state.materialize_lazy_flags();
            oracle.materialize_lazy_flags();
            EXPECT_EQ(state.flags.rflags & ~ignore_flags,
                      oracle.flags.rflags & ~ignore_flags)
                << "RFLAGS seed=" << std::hex << seed_value;
        }
    }
};
}  // namespace vitreo::cpu::jit::test::ir_x64_memory
#endif
