// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_ir_x64_narrow_mov.cpp
/// @brief Long-mode IR emission: narrow MOV partial-register writes (M11b).

// These tests execute emitted ARM64, so Intel hosts must not reference the
// native-only IR backend through this translation unit.
#if defined(__aarch64__)
#include "ir_x64_memory_fixture.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test::ir_x64_memory;

namespace {
// =============================================================================
// Long-mode narrow MOV: partial-register writes (M11b)
// =============================================================================

// A narrow write merges a field and leaves the rest of the architectural
// register alone. Every case here seeds all sixteen registers apart, so a
// lowering that truncated the destination, named the wrong register, or wrote
// the wrong field is visible as a diverged GPR rather than a diverged flag.
class IrEmit64NarrowMov : public IrEmit64Memory {
protected:
    void expect_narrow_matches_interpreter(const std::vector<u8>& bytes,
                                           unsigned instructions = 1) {
        SCOPED_TRACE(testing::Message() << bytes.size() << " bytes");
        jit = std::make_unique<JitCompiler>(memory);
        auto* native = compile(bytes);
        ASSERT_NE(native, nullptr);
        for (const u64 seed_value : {0ull, 0x0123456789ABCDEFull,
                                     0xFFFFFFFFFFFFFFFFull, 0x8000000000000080ull}) {
            auto oracle = seed();
            for (u8 index = 0; index < 16; ++index)
                oracle.gpr.set64(index, seed_value ^ (0x1111111111111111ull * index));
            oracle.gpr.set64(0, kData);   // RAX addresses the scratch page
            auto state = oracle;
            ASSERT_TRUE(memory.write_u64(kData, 0xA5A5A5A5A5A5A5A5ull).is_ok());
            {
                Interpreter interpreter(oracle, memory);
                for (unsigned step = 0; step < instructions; ++step)
                    ASSERT_EQ(interpreter.step(), ExecResult::Continue);
            }
            const auto oracle_memory = memory.read_u64(kData);
            ASSERT_TRUE(memory.write_u64(kData, 0xA5A5A5A5A5A5A5A5ull).is_ok());
            ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
            EXPECT_EQ(memory.read_u64(kData).value_or(1), oracle_memory.value_or(2))
                << "guest memory diverged, seed=" << std::hex << seed_value;
            for (u8 index = 0; index < 16; ++index)
                EXPECT_EQ(state.gpr.get64(index), oracle.gpr.get64(index))
                    << "GPR " << unsigned(index) << " seed=" << std::hex << seed_value;
            state.materialize_lazy_flags();
            oracle.materialize_lazy_flags();
            EXPECT_EQ(state.flags.rflags, oracle.flags.rflags)
                << "MOV defines no flags";
        }
    }
};

TEST_F(IrEmit64NarrowMov, ByteRegisterToRegisterInBothSpellings) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    // 88 /r: mov r/m8, r8. Without REX, indices 4-7 are AH/CH/DH/BH; with a
    // REX they are SPL/BPL/SIL/DIL. Both spellings of the same ModRM byte must
    // move different architectural registers.
    for (u8 reg = 0; reg < 8; ++reg) {
        for (u8 rm = 0; rm < 8; ++rm) {
            const u8 modrm = static_cast<u8>(0xC0 | (reg << 3) | rm);
            expect_narrow_matches_interpreter({0x88, modrm});
            expect_narrow_matches_interpreter({0x40, 0x88, modrm});
        }
    }
}

TEST_F(IrEmit64NarrowMov, WordAndExtendedByteRegisters) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    for (u8 reg = 0; reg < 8; ++reg) {
        // 66 89 /r: mov r/m16, r16.
        expect_narrow_matches_interpreter(
            {0x66, 0x89, static_cast<u8>(0xC0 | (reg << 3) | ((reg + 3) & 7))});
        // REX.R/REX.B reaching R8B-R15B and R8W-R15W.
        expect_narrow_matches_interpreter(
            {0x45, 0x88, static_cast<u8>(0xC0 | (reg << 3) | ((reg + 5) & 7))});
        expect_narrow_matches_interpreter(
            {0x66, 0x45, 0x89, static_cast<u8>(0xC0 | (reg << 3) | ((reg + 1) & 7))});
    }
}

TEST_F(IrEmit64NarrowMov, ImmediateAndMemoryForms) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    for (const u8 immediate : {u8{0x00}, u8{0x7F}, u8{0x80}, u8{0xFF}}) {
        // B0+rb: mov r8, imm8 into CL, and C6 /0 into byte [rax].
        expect_narrow_matches_interpreter({0xB1, immediate});
        expect_narrow_matches_interpreter({0xC6, 0x00, immediate});
        // A high byte destination: mov ch, imm8.
        expect_narrow_matches_interpreter({0xB5, immediate});
    }
    for (const u16 immediate : {u16{0x0000}, u16{0x7FFF}, u16{0x8000}, u16{0xFFFF}}) {
        const u8 low = static_cast<u8>(immediate);
        const u8 high = static_cast<u8>(immediate >> 8);
        expect_narrow_matches_interpreter({0x66, 0xB9, low, high});   // mov cx, imm16
        expect_narrow_matches_interpreter({0x66, 0xC7, 0x00, low, high});
    }
    // Loads and stores at both narrow widths, including a high-byte operand.
    expect_narrow_matches_interpreter({0x8A, 0x08});          // mov cl, [rax]
    expect_narrow_matches_interpreter({0x8A, 0x28});          // mov ch, [rax]
    expect_narrow_matches_interpreter({0x88, 0x08});          // mov [rax], cl
    expect_narrow_matches_interpreter({0x88, 0x28});          // mov [rax], ch
    expect_narrow_matches_interpreter({0x66, 0x8B, 0x08});    // mov cx, [rax]
    expect_narrow_matches_interpreter({0x66, 0x89, 0x08});    // mov [rax], cx
}

// A narrow write in the middle of a block must not disturb the flags the
// previous instruction published, and the register it merges into must be the
// architectural one the high-byte space names.
TEST_F(IrEmit64NarrowMov, NarrowWriteBetweenFlagProducerAndConsumer) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    // test rbx,rbx ; mov ch, cl ; mov [rax], ch
    // The narrow write deliberately does NOT touch RAX: merging into the
    // address register would move the store off the mapped page and turn a
    // flag test into a fault test.
    expect_narrow_matches_interpreter({0x48,0x85,0xDB, 0x88,0xCD, 0x88,0x28}, 3);
}

// A faulting narrow store commits nothing: not the memory, and not a partial
// register write that the same instruction would have made.
TEST_F(IrEmit64NarrowMov, FaultingNarrowStoreCommitsNothing) {
    jit = std::make_unique<JitCompiler>(memory);
    auto* native = compile({0x88, 0x08});  // mov [rax], cl
    ASSERT_NE(native, nullptr);
    auto oracle = seed();
    oracle.gpr.rax = 0x0000'4000'0000'0000ull;
    oracle.gpr.rcx = 0x1234567890ABCDEFull;
    auto state = oracle;
    (void)jit->execute(*native, state);
    ASSERT_TRUE(state.pending_raise.active);
    EXPECT_EQ(state.pending_raise.code, 0xC0000005u);
    ASSERT_GE(state.pending_raise.num_args, 2u);
    EXPECT_EQ(state.pending_raise.args[0], 1u);  // write attempted
    EXPECT_EQ(state.pending_raise.args[1], oracle.gpr.rax);
    EXPECT_EQ(state.rip, oracle.rip);
    for (u8 index = 0; index < 16; ++index)
        EXPECT_EQ(state.gpr.get64(index), oracle.gpr.get64(index)) << unsigned(index);
}
} // namespace
#endif
