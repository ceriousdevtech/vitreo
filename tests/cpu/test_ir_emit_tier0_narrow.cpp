// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_ir_emit_tier0_narrow.cpp
/// @brief Tier 1 IR emission parity: Tier 0 partial registers; contract in test_ir_emit.cpp.

#include "ir_emit_test_support.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test::ir_emit;

// TIER 0 partial-register parity.
//
// Written after a Sims 3 fault whose signature was a pointer shifted left by
// eight bits: the stack held 0x00d928f4 and ECX held 0xd928f400, which the
// guest then dereferenced. A value landing one byte position off is what a
// wrong high/low byte register does, and Tier 1 is default-off so the code
// that produced it was Tier 0's.
//
// These run on BOTH tiers deliberately — no REQUIRE_IR_GATE — so the default
// suite covers the stencil path. Every existing narrow test asserts
// `ir_emit_active()` and therefore says nothing about Tier 0.
namespace {

void expect_any_tier_matches_interpreter(const std::vector<u8>& code,
                                         const char* what) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000,
                                  MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));
    ASSERT_TRUE(memory.write_u32(0x2000, 0x00d9'28f4u));

    const auto seed = [](CpuState& cpu) {
        cpu.init_protected_mode();
        // Byte-distinct in every lane, so a value landing one byte position
        // off is a different number rather than the same one.
        cpu.gpr.set32(0, 0x1122'33F4u);
        cpu.gpr.set32(1, 0x00d9'28f4u);
        cpu.gpr.set32(2, 0x2000);
        cpu.gpr.set32(3, 0xAABB'CC98u);
        cpu.gpr.set32(4, 0x3000);
        cpu.gpr.set32(5, 0x5566'7788u);
        cpu.gpr.set32(6, 0x99AA'BBCCu);
        cpu.gpr.set32(7, 0xDDEE'FF00u);
        cpu.rip = kCode;
    };

    CpuState oracle;
    seed(oracle);
    BasicBlock block = decode_bytes(code);
    {
        Interpreter interp(oracle, memory);
        for (u32 i = 0; i < block.instruction_count; ++i) {
            ASSERT_EQ(interp.step(), ExecResult::Continue) << what;
        }
    }
    oracle.materialize_lazy_flags();

    CpuState jit_cpu;
    seed(jit_cpu);
    JitCompiler jit(memory);
    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr) << what;
    jit.execute(*compiled, jit_cpu);
    jit_cpu.materialize_lazy_flags();

    for (u8 r = 0; r < 8; ++r) {
        EXPECT_EQ(jit_cpu.gpr.get32(r), oracle.gpr.get32(r))
            << what << ": GPR " << static_cast<int>(r) << " (tier "
            << (jit.ir_emit_active() ? "1" : "0") << ")";
    }
}

}  // namespace

TEST(TierZeroNarrow, HighByteWritesLandInBits15To8) {
    expect_any_tier_matches_interpreter({0xB5, 0x5A}, "mov ch, 0x5a");
    expect_any_tier_matches_interpreter({0x88, 0xC5}, "mov ch, al");
    expect_any_tier_matches_interpreter({0x88, 0xE9}, "mov cl, ch");
    expect_any_tier_matches_interpreter({0x8A, 0xEB}, "mov ch, bl");
}

TEST(TierZeroNarrow, LowByteWritesKeepUpperBits) {
    expect_any_tier_matches_interpreter({0xB1, 0x5A}, "mov cl, 0x5a");
    expect_any_tier_matches_interpreter({0x88, 0xC1}, "mov cl, al");
    expect_any_tier_matches_interpreter({0x8A, 0xCB}, "mov cl, bl");
}

TEST(TierZeroNarrow, ByteLoadsAndStores) {
    expect_any_tier_matches_interpreter({0x8A, 0x0A}, "mov cl, [edx]");
    expect_any_tier_matches_interpreter({0x8A, 0x2A}, "mov ch, [edx]");
    expect_any_tier_matches_interpreter({0x88, 0x0A}, "mov [edx], cl");
    expect_any_tier_matches_interpreter({0x88, 0x2A}, "mov [edx], ch");
}

TEST(TierZeroNarrow, WordFormsKeepTheUpperHalf) {
    expect_any_tier_matches_interpreter({0x66, 0xB9, 0x34, 0x12},
                                        "mov cx, 0x1234");
    expect_any_tier_matches_interpreter({0x66, 0x89, 0xC1}, "mov cx, ax");
    expect_any_tier_matches_interpreter({0x66, 0x8B, 0x0A}, "mov cx, [edx]");
}

TEST(TierZeroNarrow, MovzxMovsxIntoThirtyTwoBits) {
    expect_any_tier_matches_interpreter({0x0F, 0xB6, 0xCB}, "movzx ecx, bl");
    expect_any_tier_matches_interpreter({0x0F, 0xB6, 0xCF}, "movzx ecx, bh");
    expect_any_tier_matches_interpreter({0x0F, 0xBE, 0xCF}, "movsx ecx, bh");
    expect_any_tier_matches_interpreter({0x0F, 0xB7, 0xCB}, "movzx ecx, bx");
    expect_any_tier_matches_interpreter({0x0F, 0xB6, 0x0A}, "movzx ecx,[edx]");
}

// THE shape from the fault: load a pointer from memory into a register, then
// use it. If any part of that path shifts the value by a byte the pointer is
// wrong by exactly the amount observed.
TEST(TierZeroNarrow, PointerLoadThroughByteOpsIsNotShifted) {
    expect_any_tier_matches_interpreter({0x8B, 0x0A}, "mov ecx, [edx]");
    expect_any_tier_matches_interpreter({0x8B, 0x0A,        // mov ecx,[edx]
                                         0x88, 0xE9},       // mov cl, ch
                                        "load then byte-move");
    expect_any_tier_matches_interpreter({0x0F, 0xB6, 0x0A,  // movzx ecx,[edx]
                                         0x0F, 0xB6, 0x2A}, // movzx ebp,[edx]
                                        "two byte loads");
}
