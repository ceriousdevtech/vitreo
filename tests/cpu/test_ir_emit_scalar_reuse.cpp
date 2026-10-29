// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_ir_emit_scalar_reuse.cpp
/// @brief Tier 1 IR emission parity: long-mode scalars and value reuse; contract in test_ir_emit.cpp.

#include "ir_emit_test_support.hpp"
#include "ir_tier1_first_compile.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test::ir_emit;

namespace {

/// Does a block address invocation-local spill storage? X3 is the backend's
/// spill-address scratch, reloaded from the JitContext pin X20 before every
/// slot access.
bool ir_addresses_spill_frame(const CompiledBlock& compiled) {
    const u32* words = static_cast<const u32*>(compiled.code);
    const u32 count = compiled.code_size / 4u;
    const u32 load_frame = arm64::ldr_x_uoff(3, 20, JIT_CTX_OFF_IR_SPILL_FRAME >> 3);
    bool found = false;
    for (u32 i = 0; i < count; ++i) found |= words[i] == load_frame;
    return found;
}

}  // namespace

// Intel SDM Vol. 1 3.4.1.1 and Vol. 2B MOV: cover every long-mode GPR,
// including unpinned registers, full-width immediates and upper-half clearing.
TEST(IrEmit64, RegisterMovesMatchInterpreter) {
    const vitreo::cpu::jit::test::IrTier1FirstCompile tier1_first_compile;
    if (!JitCompiler::ir_enabled_for(true)) GTEST_SKIP() << "requires VITREO_JIT_IR=all";
    std::vector<std::vector<u8>> bodies;
    for (u8 reg = 0; reg < 16; ++reg) {
        bodies.push_back({static_cast<u8>(0x48 | (reg >= 8)),
            static_cast<u8>(0xB8 + (reg & 7)),
            0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x81});
        bodies.push_back({static_cast<u8>(0x48 | (reg >= 8)), 0xC7,
            static_cast<u8>(0xC0 | (reg & 7)), 0x01, 0x00, 0x00, 0x80});
        const u8 src = static_cast<u8>((reg + 5) & 15);
        for (const u8 width : {u8{0}, u8{8}}) {
            bodies.push_back({static_cast<u8>(0x40 | width |
                (src >= 8 ? 4 : 0) | (reg >= 8 ? 1 : 0)),
                0x89, static_cast<u8>(0xC0 | ((src & 7) << 3) | (reg & 7))});
        }
        bodies.push_back({static_cast<u8>(0x40 | (reg >= 8)),
            static_cast<u8>(0xB8 + (reg & 7)), 0xEF, 0xCD, 0xAB, 0x89});
    }
    // Exercise repeated writes/read-after-write across guest instructions.
    bodies.push_back({0x49,0xB8,1,2,3,4,5,6,7,8,
                      0x4D,0x89,0xC7, 0x45,0x89,0xF9, 0x4C,0x89,0xC8});
    expect_long_mode_scalar(bodies);
}

TEST(IrEmit64, RegisterAluMatchesInterpreter) {
    const vitreo::cpu::jit::test::IrTier1FirstCompile tier1_first_compile;
    if (!JitCompiler::ir_enabled_for(true)) GTEST_SKIP() << "requires VITREO_JIT_IR=all";
    std::vector<std::vector<u8>> bodies;
    for (u8 dst = 0; dst < 16; ++dst) {
        for (const u8 width : {u8{0}, u8{8}}) {
            for (const u8 opcode : {u8{0x01},u8{0x29},u8{0x21},u8{0x09},u8{0x31},u8{0x39},u8{0x85}}) {
                for (const u8 src : {dst, static_cast<u8>((dst + 5) & 15)}) {
                    bodies.push_back({static_cast<u8>(0x40 | width |
                        (src >= 8 ? 4 : 0) | (dst >= 8 ? 1 : 0)), opcode,
                        static_cast<u8>(0xC0 | ((src & 7) << 3) | (dst & 7))});
                }
            }
            for (const u8 extension : {u8{0},u8{1},u8{4},u8{5},u8{6},u8{7}}) {
                bodies.push_back({static_cast<u8>(0x40 | width | (dst >= 8)),
                    0x83, static_cast<u8>(0xC0 | (extension << 3) | (dst & 7)), 0xFF});
                bodies.push_back({static_cast<u8>(0x40 | width | (dst >= 8)),
                    0x81, static_cast<u8>(0xC0 | (extension << 3) | (dst & 7)),
                    0x01,0x00,0x00,0x80});
            }
        }
    }
    // Earlier flags are dead; final TEST is live across the block exit.
    bodies.push_back({0x48,0x01,0xC8, 0x49,0x29,0xC0, 0x4D,0x85,0xC0});
    expect_long_mode_scalar(bodies);
}

TEST(IrEmit, CrossInstructionPinnedReadReuse) {
    REQUIRE_IR_GATE();
    // ECX survives both reads; the second form overwrites AH between reads,
    // which must invalidate the previous full EAX alias.
    expect_tier1_matches_interpreter({0x89,0xC8, 0x01,0xCA, 0x29,0xCB}, true);
    expect_tier1_matches_interpreter({0x89,0xC2, 0xB4,0x77, 0x89,0xC3}, true);
    // Helpers preserve the architectural pins; reuse remains valid through
    // a guest-memory load, including the region-cache helper path.
    expect_tier1_matches_interpreter_with_memory({0x89,0xCA, 0x8B,0x01, 0x89,0xCB}, true);
}

TEST(IrEmit64, MemoryMovesPreserveFullAddressesAndFaultState) {
    const vitreo::cpu::jit::test::IrTier1FirstCompile tier1_first_compile;
    if (!JitCompiler::ir_enabled_for(true)) GTEST_SKIP() << "requires VITREO_JIT_IR=all";
    constexpr GuestAddr code_address = 0x100001000ull;
    constexpr GuestAddr data_address = 0x200002000ull;
    for (const bool wide : {false, true}) {
        for (const bool store : {false, true}) {
            // 0=success,1=unmapped,2=read-only,3=cross unmapped page boundary.
            for (const u32 scenario : {0u, 1u, 2u, 3u}) {
                GuestMemory memory;
                ASSERT_TRUE(memory.map_region(code_address, 0x1000, MemoryProtection::ReadWrite));
                ASSERT_TRUE(memory.map_region(data_address, 0x1000, MemoryProtection::ReadWrite));
                ASSERT_TRUE(memory.write_u64(data_address + 16, 0xFEDCBA9876543210ull));
                const u64 offset = scenario == 3 ? 0xFFEu : 16u;
                const GuestAddr base = scenario == 1 ? 0x300003000ull : data_address;
                // CMP establishes the flag state the faulting MOV must expose;
                // a later ADD must not make those flags dead across the access.
                std::vector<u8> code{0x48,0x39,0xD8,
                    static_cast<u8>((wide ? 0x48 : 0x40) | (store ? 4 : 0)),
                    static_cast<u8>(store ? 0x89 : 0x8B),
                    static_cast<u8>(store ? 0x81 : 0x91),
                    static_cast<u8>(offset), static_cast<u8>(offset >> 8),0,0,
                    0x48,0x01,0xD8};
                ASSERT_TRUE(memory.write_bytes(code_address, code.data(), code.size()));
                if (scenario == 2)
                    ASSERT_TRUE(memory.protect_region(data_address, 0x1000, MemoryProtection::Read));
                BasicBlock block;
                block.start_addr = code_address;
                block.is_64bit = true;
                MemoryByteReader reader(code.data(), code.size());
                while (reader.remaining()) {
                    DecodedInstruction instruction;
                    ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode64, instruction), DecodeError::Success);
                    block.instructions.push_back(instruction);
                }
                block.byte_length = static_cast<u32>(code.size());
                block.instruction_count = static_cast<u32>(block.instructions.size());
                CpuState expected;
                expected.init_long_mode();
                expected.rip = code_address;
                expected.gpr.set64(0, 0x7FFFFFFFFFFFFFFFull);
                expected.gpr.set64(3, 0xFFFFFFFFFFFFFFFFull);
                expected.gpr.set64(1, base);
                expected.gpr.set64(2, 0xA5A5A5A5A5A5A5A5ull);
                expected.gpr.set64(8, 0x1234567887654321ull);
                CpuState actual = expected;
                Interpreter interpreter(expected, memory);
                const bool faults = scenario == 1 || scenario == 3 || (store && scenario == 2);
                ASSERT_EQ(interpreter.step(), ExecResult::Continue);
                if (!faults) {
                    ASSERT_EQ(interpreter.step(), ExecResult::Continue);
                    ASSERT_EQ(interpreter.step(), ExecResult::Continue);
                }
                const auto expected_memory = memory.read_u64(data_address + 16);
                ASSERT_TRUE(expected_memory.is_ok());
                if (scenario != 2)
                    ASSERT_TRUE(memory.write_u64(data_address + 16, 0xFEDCBA9876543210ull));
                JitCompiler jit(memory);
                ASSERT_TRUE(jit.ir_build_block(block).ok) << ir::decline_reason_name(jit.ir_last_result().reason);
                auto* compiled = jit.compile(block);
                ASSERT_NE(compiled, nullptr);
                ASSERT_TRUE(jit.ir_emit_active()) << ir::decline_reason_name(jit.ir_last_result().reason);
                jit.execute(*compiled, actual);
                EXPECT_EQ(actual.pending_raise.active, faults);
                EXPECT_EQ(actual.rip, expected.rip);
                for (u8 reg = 0; reg < 16; ++reg)
                    EXPECT_EQ(actual.gpr.get64(reg), expected.gpr.get64(reg));
                actual.materialize_lazy_flags();
                expected.materialize_lazy_flags();
                EXPECT_EQ(actual.flags.rflags, expected.flags.rflags);
                const auto observed = memory.read_u64(data_address + 16);
                ASSERT_TRUE(observed.is_ok());
                EXPECT_EQ(observed.value(), expected_memory.value());
            }
        }
    }
}

TEST(IrEmit64, DirectBranchesUseFullRipAndArchitecturalFlags) {
    const vitreo::cpu::jit::test::IrTier1FirstCompile tier1_first_compile;
    if (!JitCompiler::ir_enabled_for(true)) GTEST_SKIP() << "requires VITREO_JIT_IR=all";
    std::vector<std::vector<u8>> bodies;
    for (u8 cond = 0; cond < 16; ++cond) {
        bodies.push_back({static_cast<u8>(0x70 | cond), 0x10});
        bodies.push_back({0x0F, static_cast<u8>(0x80 | cond), 0x80,0xFF,0xFF,0xFF});
        for (const u8 width : {u8{0x40}, u8{0x48}}) {
            bodies.push_back({width,0x39,0xC8, static_cast<u8>(0x70 | cond),0x10});
            bodies.push_back({width,0x85,0xC8, 0x0F,static_cast<u8>(0x80 | cond),0x80,0xFF,0xFF,0xFF});
        }
    }
    bodies.push_back({0xEB,0x80});
    bodies.push_back({0xE9,0x00,0x00,0x01,0x00});
    expect_long_mode_scalar(bodies);
}

TEST(IrEmit, CommittedComputedValuesSurviveFollowingInstructions) {
    REQUIRE_IR_GATE();
    expect_tier1_matches_interpreter({0x01,0xC8, 0x01,0xC2}, true);
    // Bind computed EAX, copy it, overwrite EAX, then use the retained copy.
    expect_tier1_matches_interpreter({0x01,0xC8, 0x89,0xC2,
                                      0xB8,1,0,0,0, 0x89,0xD3}, true);
    expect_tier1_matches_interpreter({0x01,0xC8, 0xB4,0x77, 0x89,0xC3}, true);
    // A computed EDX survives a may-fault access through its committed pin.
    expect_tier1_matches_interpreter_with_memory({0x83,0xC2,1,
                                                 0x8B,0x01, 0x01,0xD0}, true);
}

TEST(IrEmit, CommittedComputedValueIsVisibleAtFollowingMemoryFault) {
    REQUIRE_IR_GATE();
    GuestMemory memory;
    const std::vector<u8> code{0x83,0xC2,1, 0x8B,0x01, 0x01,0xD0};
    ASSERT_TRUE(memory.map_region(kCode, 0x1000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));
    CpuState expected;
    expected.init_protected_mode();
    expected.rip = kCode;
    expected.gpr.set32(0, 0x12345678);
    expected.gpr.set32(1, 0xDEAD0000);
    expected.gpr.set32(2, 0x7FFFFFFF);
    CpuState actual = expected;
    Interpreter interpreter(expected, memory);
    ASSERT_EQ(interpreter.step(), ExecResult::Continue);
    JitCompiler jit(memory);
    auto block = decode_bytes(code);
    ASSERT_TRUE(jit.ir_build_block(block).ok);
    auto* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr);
    ASSERT_TRUE(jit.ir_emit_active());
    jit.execute(*compiled, actual);
    ASSERT_TRUE(actual.pending_raise.active);
    EXPECT_EQ(actual.rip, expected.rip);
    for (u8 reg = 0; reg < 8; ++reg)
        EXPECT_EQ(actual.gpr.get32(reg), expected.gpr.get32(reg));
    actual.materialize_lazy_flags();
    expected.materialize_lazy_flags();
    EXPECT_EQ(actual.flags.rflags, expected.flags.rflags);
}

TEST(IrEmit, BlockScalarConstantsRemainLiveAcrossRegisterPermutations) {
    REQUIRE_IR_GATE();
    for (const u8 first : {u8{0},u8{1},u8{2},u8{3},u8{6},u8{7}}) {
        const u8 second = static_cast<u8>((first + 1) & 3);
        const u8 third = static_cast<u8>((second + 1) & 3);
        for (const u8 immediate : {u8{1},u8{5},u8{0x7F},u8{0x80},u8{0xFF}}) {
            const std::vector<u8> body{
                0x83, static_cast<u8>(0xC0 | first), immediate,
                0x89, static_cast<u8>(0xC0 | (first << 3) | second),
                static_cast<u8>(0xB8 | first), 0x67,0x45,0x23,0x81,
                0x83, static_cast<u8>(0xE8 | third), immediate,
                0x31, static_cast<u8>(0xC0 | (second << 3) | first),
                0x83, static_cast<u8>(0xC0 | second), immediate};
            expect_tier1_matches_interpreter(body, true);
        }
    }
    // With a memory helper or CF merge, constants stay instruction-local.
    expect_tier1_matches_interpreter_with_memory({0x83,0xC0,5,
        0x8B,0x11, 0x83,0xC0,5}, true);
    expect_tier1_matches_interpreter({0x83,0xC0,5, 0x42, 0x83,0xC0,5}, true);
}

// Repeated immediates across instructions must not become block-long values:
// three of them outlive the three-register pool, and the fallback then spills
// every value in the block. Each instruction keeps its own constant, the block
// stays on the scalar emitter, and nothing touches the spill frame. Parity
// first, because a block that computes the wrong answer would satisfy the
// encoding scan on its own.
TEST(IrEmit, BlockScalarConstantsStayInstructionLocalAndNeverSpill) {
    REQUIRE_IR_GATE();
    const std::vector<u8> body{0x83,0xC0,1, 0x83,0xC3,2, 0x83,0xC2,3,
                              0x83,0xC0,1, 0x83,0xC3,2, 0x83,0xC2,3};
    expect_tier1_matches_interpreter(body, true);
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(kCode, 0x1000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.write_bytes(kCode, body.data(), body.size()));
    JitCompiler jit(memory);
    auto block = decode_bytes(body);
    ASSERT_TRUE(jit.ir_build_block(block).ok);
    usize constants = 0;
    for (const auto& in : jit.ir_block().insts)
        constants += in.op == ir::Op::Const;
    EXPECT_EQ(constants, 6u);
    EXPECT_FALSE(ir::requires_snapshot_backing(jit.ir_block()));
    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr);
    EXPECT_TRUE(jit.ir_emit_active());
    EXPECT_FALSE(ir_addresses_spill_frame(*compiled));
}
